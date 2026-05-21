#!/usr/bin/env bash
# Phase 3 Sprint 7: initiative audit HTML exporter integration test.
#
# The exporter is the v1.0 self-improvement demo feature. This test verifies:
#   - wt-task initiative report INIT --out FILE writes a self-contained HTML doc
#   - the report contains the initiative id, a task row, and a deliverable
#   - data is HTML-escaped (a task title containing <script> renders inert)
#   - a secret-scan hit surfaces in the report
#   - an unknown initiative exits non-zero
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((28000 + RANDOM % 1000))}"
CONFIG="$TMPDIR/woventeam.conf"
ROOM_URL="http://127.0.0.1:$PORT"

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]]; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf "$TMPDIR"
}
trap cleanup EXIT

mkdir -p "$TMPDIR/data" "$TMPDIR/runtime/tasks" "$TMPDIR/deliverables"
cat >"$TMPDIR/secret-patterns.txt" <<'PATTERNS'
github-pat=ghp_[A-Za-z0-9]{36}
PATTERNS
cat >"$CONFIG" <<CFG
roomName=report
roomLogPath=$TMPDIR/data/phase0-room.jsonl
taskLedgerPath=$TMPDIR/data/task-packages.jsonl
taskProjectionDbPath=$TMPDIR/data/task-projection.sqlite
httpBindAddress=127.0.0.1
httpPort=$PORT
fsyncEachMessage=0
roleRoutingEnabled=1
maxActiveTasksPerAgent=8
maxSubtasksPerParent=8
maxTasksPerInitiative=20
runtimeRootPath=$TMPDIR/runtime/tasks
deliverableRoot=$TMPDIR/deliverables
secretScanPatternsFile=$TMPDIR/secret-patterns.txt
operatorSessionPath=$TMPDIR/data/operator-session.token
CFG

"$ROOT/build/wt-roomd" --config "$CONFIG" >"$TMPDIR/server.log" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 50); do
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then cat "$TMPDIR/server.log" >&2; echo "daemon exited" >&2; exit 1; fi
  if curl -fsS "$ROOM_URL/api/health" >/dev/null 2>&1; then break; fi
  sleep 0.1
done

export WT_ROOM_URL="$ROOM_URL"
initiative="init_report"

# Task with a markup-bearing title to prove the renderer escapes data.
resp="$("$ROOT/bin/wt-task" create \
  --title 'Audit exporter <script>alert(1)</script>' \
  --body 'Build the initiative report feature.' \
  --role backend_dev --agent chatgpt --initiative "$initiative" --max-tokens 100)"
task="$(jq -r '.taskId' <<<"$resp")"
test "$task" != "null"

# Ship a clean deliverable so the report's deliverables section is populated.
mkdir -p "$TMPDIR/runtime/tasks/$task"
printf 'The exported report.\n' > "$TMPDIR/runtime/tasks/$task/result.md"
"$ROOT/bin/wt-task" artifact promote "$task" --path result.md --reviewer ceo --notes "ship it" >/dev/null
"$ROOT/bin/wt-task" artifact ship "$task" --mode copy --reviewer ceo >/dev/null

# Plant a secret on a second task + ship as copy so a secret_scan record exists.
resp2="$("$ROOT/bin/wt-task" create --title "Secret bearing" --body "b" \
  --role backend_dev --agent chatgpt --initiative "$initiative" --max-tokens 100)"
task2="$(jq -r '.taskId' <<<"$resp2")"
mkdir -p "$TMPDIR/runtime/tasks/$task2"
printf 'token ghp_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n' > "$TMPDIR/runtime/tasks/$task2/result.md"
"$ROOT/bin/wt-task" artifact promote "$task2" --path result.md --reviewer ceo --notes "ok" >/dev/null
"$ROOT/bin/wt-task" artifact ship "$task2" --mode copy --reviewer ceo >/dev/null

# --- Render the report ---
"$ROOT/bin/wt-task" initiative report "$initiative" --out "$TMPDIR/report.html" 2>/dev/null
report="$TMPDIR/report.html"
test -f "$report"

head -1 "$report" | grep -qi '<!doctype html>'
grep -q "$initiative" "$report"
grep -q "$task" "$report"
# Title markup must be escaped, never present raw.
grep -q "&lt;script&gt;" "$report"
if grep -q "<script>alert(1)</script>" "$report"; then
  echo "raw script injection in report" >&2; exit 1
fi
# Deliverable + secret-scan sections populated.
grep -qi "deliverable" "$report"
grep -qi "secret scan" "$report"
grep -q "match" "$report"   # the planted token's scan pill

# --- stdout mode (no --out) also produces a document ---
# Capture to a variable (not a head pipe) so the generator runs to completion
# without taking SIGPIPE under pipefail.
stdout_report="$("$ROOT/bin/wt-task" initiative report "$initiative" 2>/dev/null)"
grep -qi '<!doctype html>' <<<"$stdout_report"

# --- unknown initiative exits non-zero ---
if "$ROOT/bin/wt-task" initiative report "init_does_not_exist" >/dev/null 2>&1; then
  echo "report of unknown initiative should fail" >&2; exit 1
fi

echo "wt-initiative-report integration test passed"
