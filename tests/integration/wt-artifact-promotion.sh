#!/usr/bin/env bash
# Phase 2 Sprint 4: artifact promotion integration test.
#
# Walks an artifact through the full lifecycle (draft -> reviewed -> accepted),
# verifies the projection surfaces the resulting state on /api/task-detail and
# the per-initiative inventory on /api/initiative-artifacts, and exercises the
# CLI wrapper plus the rejected path on a second task.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((28000 + RANDOM % 1000))}"
CONFIG="$TMPDIR/woventeam.conf"
ROOM_LOG="$TMPDIR/data/phase0-room.jsonl"
TASK_LEDGER="$TMPDIR/data/task-packages.jsonl"
PROJECTION="$TMPDIR/data/task-projection.sqlite"
RUNTIME="$TMPDIR/runtime/tasks"
SERVER_LOG="$TMPDIR/wt-roomd.log"
ROOM_URL="http://127.0.0.1:$PORT"

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]]; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf "$TMPDIR"
}
trap cleanup EXIT

mkdir -p "$TMPDIR/data" "$RUNTIME"
cat >"$CONFIG" <<CFG
roomName=artifact_promotion
roomLogPath=$ROOM_LOG
taskLedgerPath=$TASK_LEDGER
taskProjectionDbPath=$PROJECTION
httpBindAddress=127.0.0.1
httpPort=$PORT
fsyncEachMessage=0
roleRoutingEnabled=1
maxActiveTasksPerAgent=8
maxSubtasksPerParent=8
maxTasksPerInitiative=20
runtimeRootPath=$RUNTIME
CFG

"$ROOT/build/wt-roomd" --config "$CONFIG" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 50); do
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    cat "$SERVER_LOG" >&2 || true
    echo "wt-roomd exited before health check" >&2
    exit 1
  fi
  if curl -fsS "$ROOM_URL/api/health" >/dev/null 2>&1; then break; fi
  sleep 0.1
done

initiative="init_artifact_promotion"

# --- Task 1: walk full lifecycle draft -> reviewed -> accepted ---
task1_response="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" create \
    --title "Artifact lifecycle task" \
    --body "Validate the full artifact promotion lifecycle." \
    --role backend_dev \
    --agent chatgpt \
    --initiative "$initiative" \
    --max-tokens 100
)"
task1="$(jq -r '.taskId' <<<"$task1_response")"
test "$task1" != "null"

# Create a workspace result so the export path returns content.
mkdir -p "$RUNTIME/$task1"
printf 'Phase 2 sprint 4 deliverable\n' > "$RUNTIME/$task1/result.md"

# Draft directly via the API to exercise the HTTP surface.
curl -fsS -H 'Content-Type: application/json' \
  -d "{\"taskId\":\"$task1\",\"state\":\"draft\",\"reviewer\":\"qa\",\"notes\":\"first pass\",\"artifactPath\":\"result.md\"}" \
  "$ROOM_URL/api/task-artifact" | jq -e '.ok == true and .state == "draft"' >/dev/null

# Reviewed via the CLI verb.
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" artifact review "$task1" \
  --path result.md --reviewer qa --notes "ran spot checks" >/dev/null

# Promote via the CLI verb (maps to state=accepted).
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" artifact promote "$task1" \
  --path result.md --reviewer ceo --notes "looks good, shipping" >/dev/null

# Verify the projection surfaces the accepted state with reviewer + notes + path.
curl -fsS "$ROOM_URL/api/task-detail?taskId=$task1" |
  jq -e '
    .task.artifactState == "accepted" and
    .task.lastReviewer == "ceo" and
    .task.lastReviewNotes == "looks good, shipping" and
    .task.acceptedArtifactPath == "result.md" and
    .task.acceptedAtUnixMs > 0 and
    ([.events[] | select(.eventType == "artifact" and .status == "")] | length) >= 0 and
    ([.events[] | select(.eventType == "artifact")] | length) == 3
  ' >/dev/null

# Verify the export path streams the workspace result text.
exported="$(WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" artifact export "$task1")"
test "$exported" = "Phase 2 sprint 4 deliverable"

# --- Task 2: rejected lifecycle, then superseded ---
task2_response="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" create \
    --title "Artifact rejection task" \
    --body "Validate the artifact rejection / supersede lifecycle." \
    --role backend_dev \
    --agent chatgpt \
    --initiative "$initiative" \
    --max-tokens 100
)"
task2="$(jq -r '.taskId' <<<"$task2_response")"
test "$task2" != "null"
mkdir -p "$RUNTIME/$task2"
printf 'rejected output\n' > "$RUNTIME/$task2/result.md"

WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" artifact reject "$task2" \
  --path result.md --reviewer ceo --notes "wrong tool" >/dev/null
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" artifact supersede "$task2" \
  --path result.md --reviewer ceo --notes "replaced by task1" >/dev/null

curl -fsS "$ROOM_URL/api/task-detail?taskId=$task2" |
  jq -e '
    .task.artifactState == "superseded" and
    .task.lastReviewer == "ceo" and
    .task.lastReviewNotes == "replaced by task1" and
    .task.acceptedAtUnixMs == 0
  ' >/dev/null

# --- Initiative inventory: must show both tasks; only task1 counts as accepted ---
curl -fsS "$ROOM_URL/api/initiative-artifacts?initiativeId=$initiative" |
  jq -e --arg t1 "$task1" --arg t2 "$task2" '
    .ok == true and
    .acceptedCount == 1 and
    .pendingCount == 1 and
    ([.artifacts[] | select(.taskId == $t1 and .artifactState == "accepted")] | length) == 1 and
    ([.artifacts[] | select(.taskId == $t2 and .artifactState == "superseded")] | length) == 1
  ' >/dev/null

# --- CLI list output: TSV with state and reviewer ---
list_out="$(WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" artifact list "$initiative")"
# `printf %s` would strip the trailing newline; use echo for a robust line count.
test "$(echo "$list_out" | wc -l)" -eq 2
echo "$list_out" | grep -q "accepted"
echo "$list_out" | grep -q "superseded"

# --- Input validation: bad state is rejected, bad path is rejected ---
status="$(curl -sS -o /dev/null -w '%{http_code}' -H 'Content-Type: application/json' \
  -d "{\"taskId\":\"$task1\",\"state\":\"weird\",\"reviewer\":\"x\"}" \
  "$ROOM_URL/api/task-artifact")"
test "$status" = "400"
status="$(curl -sS -o /dev/null -w '%{http_code}' -H 'Content-Type: application/json' \
  -d "{\"taskId\":\"$task1\",\"state\":\"accepted\",\"reviewer\":\"x\",\"artifactPath\":\"../etc/passwd\"}" \
  "$ROOM_URL/api/task-artifact")"
test "$status" = "400"

echo "wt-artifact-promotion integration test passed"
