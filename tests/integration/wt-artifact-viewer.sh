#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((23000 + RANDOM % 1000))}"
ROOM_LOG="$TMPDIR/phase0-room.jsonl"
TASK_LEDGER="$TMPDIR/task-packages.jsonl"
PROJECTION="$TMPDIR/task-projection.sqlite"
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

task_id="task_artifact_viewer"
mkdir -p "$RUNTIME/$task_id"
printf 'operator-visible result\n' > "$RUNTIME/$task_id/result.md"
printf 'stdout line\n' > "$RUNTIME/$task_id/stdout.log"
printf 'stderr line\n' > "$RUNTIME/$task_id/stderr.log"
printf '{"schema":"woventeam.adapter_manifest.v0.1","taskId":"%s","adapter":"test"}\n' "$task_id" > "$RUNTIME/$task_id/manifest.json"

WT_ROOM_LOG_PATH="$ROOM_LOG" \
WT_TASK_LEDGER_PATH="$TASK_LEDGER" \
WT_TASK_PROJECTION_DB_PATH="$PROJECTION" \
WT_RUNTIME_ROOT_PATH="$RUNTIME" \
  "$ROOT/build/wt-roomd" --bind 127.0.0.1 --port "$PORT" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 50); do
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    cat "$SERVER_LOG" >&2 || true
    echo "wt-roomd exited before health check" >&2
    exit 1
  fi
  if curl -fsS "$ROOM_URL/api/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done
curl -fsS "$ROOM_URL/api/health" >/dev/null

curl -fsS "$ROOM_URL/api/task-artifacts?taskId=$task_id" |
  jq -e '
    .ok == true and
    .exists == true and
    .resultText == "operator-visible result\n" and
    .stdoutText == "stdout line\n" and
    .stderrText == "stderr line\n" and
    (.files | map(.name) | index("manifest.json")) != null
  ' >/dev/null

curl -sS "$ROOM_URL/api/task-artifacts?taskId=../bad" |
  jq -e '.ok == false' >/dev/null

echo "wt-artifact-viewer integration test passed"
