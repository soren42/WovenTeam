#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((19000 + RANDOM % 1000))}"
ROOM_LOG="$TMPDIR/phase0-room.jsonl"
TASK_LEDGER="$TMPDIR/task-packages.jsonl"
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

WT_ROOM_LOG_PATH="$ROOM_LOG" WT_TASK_LEDGER_PATH="$TASK_LEDGER" \
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

create_response="$(
    WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" create \
        --title "Sprint 2 assignment smoke" \
        --body "Claim this queued task and write status/result events." \
        --role backend_dev \
        --agent chatgpt
)"
task_id="$(jq -r '.taskId' <<<"$create_response")"
test "$task_id" != "null"

WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" list | grep -q "$task_id"
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" show "$task_id" | grep -q "Sprint 2 assignment smoke"

WT_ROOM_LOG_PATH="$ROOM_LOG" WT_TASK_LEDGER_PATH="$TASK_LEDGER" \
    "$ROOT/build/wt-agent" --agent chatgpt --once >"$TMPDIR/agent.out"

jq -e --arg taskId "$task_id" '
  [.[] | select(.taskId == $taskId and .schema == "woventeam.task_event.v0.1" and .status == "running")] | length == 1
' < <(curl -fsS "$ROOM_URL/api/tasks") >/dev/null
jq -e --arg taskId "$task_id" '
  [.[] | select(.taskId == $taskId and .schema == "woventeam.task_event.v0.1" and .status == "complete")] | length == 1
' < <(curl -fsS "$ROOM_URL/api/tasks") >/dev/null

grep -q '"messageType":"task.assign"' "$ROOM_LOG"
grep -q '"messageType":"task.status"' "$ROOM_LOG"
grep -q '"messageType":"task.result"' "$ROOM_LOG"

WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" update-status "$task_id" --status blocked --message "operator test update" >/dev/null
jq -e --arg taskId "$task_id" '
  [.[] | select(.taskId == $taskId and .schema == "woventeam.task_event.v0.1" and .status == "blocked")] | length == 1
' < <(curl -fsS "$ROOM_URL/api/tasks") >/dev/null

echo "wt-task assignment path integration test passed"
