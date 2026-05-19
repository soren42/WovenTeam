#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((20000 + RANDOM % 1000))}"
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
WT_TOKEN_DAILY_BUDGET=20000000 WT_TOKEN_MONTHLY_BUDGET=20000000 \
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

initiative="init_sprint4_smoke"
pm_response="$(
    WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" create \
        --title "CEO initiative charter" \
        --body "Create a PM-managed delivery path." \
        --role project_manager \
        --agent claude \
        --initiative "$initiative"
)"
pm_task="$(jq -r '.taskId' <<<"$pm_response")"
test "$pm_task" != "null"

worker_response="$(
    WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" request \
        --parent "$pm_task" \
        --by-role project_manager \
        --by project_manager \
        --role backend_dev \
        --agent chatgpt \
        --initiative "$initiative" \
        --tool-profile observe \
        --title "Implement manager subtask fixture" \
        --body "Produce the worker artifact for review."
)"
worker_task="$(jq -r '.taskId' <<<"$worker_response")"
test "$worker_task" != "null"

direct_task="task_direct_request_without_projection"
direct_response="$(
    jq -cn \
      --arg parentTaskId "$pm_task" \
      --arg requestedTaskId "$direct_task" \
      --arg initiativeId "$initiative" \
      --arg requestedByRole "project_manager" \
      --arg requestedRole "technical_writer" \
      --arg title "Document canonical request fixture" \
      --arg body "This request intentionally omits taskId; wt-roomd must add the native projection." \
      '{schema:"woventeam.task_request.v0.1",parentTaskId:$parentTaskId,requestedTaskId:$requestedTaskId,initiativeId:$initiativeId,requestedByRole:$requestedByRole,requestedRole:$requestedRole,title:$title,body:$body}' |
      curl -fsS -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/task-request"
)"
test "$(jq -r '.taskId' <<<"$direct_response")" = "$direct_task"

review_response="$(
    WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" request \
        --parent "$worker_task" \
        --by-role project_manager \
        --by project_manager \
        --role tester \
        --agent chatgpt \
        --initiative "$initiative" \
        --title "Review manager subtask fixture" \
        --body "Review the worker artifact after the worker task completes."
)"
review_task="$(jq -r '.taskId' <<<"$review_response")"
test "$review_task" != "null"

denied_status="$(
    jq -cn \
      --arg parentTaskId "$pm_task" \
      --arg requestedTaskId "task_denied" \
      --arg initiativeId "$initiative" \
      --arg requestedByRole "program_manager" \
      --arg requestedRole "backend_dev" \
      --arg title "Denied direct implementation spawn" \
      --arg body "Program Manager must not directly spawn implementation roles." \
      '{schema:"woventeam.task_request.v0.1",parentTaskId:$parentTaskId,requestedTaskId:$requestedTaskId,initiativeId:$initiativeId,requestedByRole:$requestedByRole,requestedRole:$requestedRole,title:$title,body:$body}' |
      curl -sS -o "$TMPDIR/denied.json" -w '%{http_code}' -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/task-request"
)"
test "$denied_status" = "403"

WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" update-status "$worker_task" --status complete --message "worker finished" >/dev/null
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" update-status "$review_task" --status complete --message "review passed" >/dev/null

blocked_parent_response="$(
    WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" create \
        --title "Blocked parent fixture" \
        --body "A parent task that will block its dependent." \
        --role backend_dev \
        --agent chatgpt \
        --initiative "$initiative"
)"
blocked_parent="$(jq -r '.taskId' <<<"$blocked_parent_response")"
blocked_child_response="$(
    WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" request \
        --parent "$blocked_parent" \
        --by-role project_manager \
        --role tester \
        --initiative "$initiative" \
        --title "Dependent blocked fixture" \
        --body "This task should be blocked when its dependency is blocked."
)"
blocked_child="$(jq -r '.taskId' <<<"$blocked_child_response")"

WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" update-status "$blocked_parent" --status blocked --message "dependency failed" >/dev/null

tasks_json="$(curl -fsS "$ROOM_URL/api/tasks")"
jq -e --arg taskId "$worker_task" '
  [.[] | select(.taskId == $taskId and .schema == "woventeam.task_request.v0.1")] | length == 1
' <<<"$tasks_json" >/dev/null
jq -e --arg taskId "$direct_task" '
  [.[] | select(.taskId == $taskId and .schema == "woventeam.task_request.v0.1" and .requestedTaskId == $taskId)] | length == 1
' <<<"$tasks_json" >/dev/null
jq -e --arg taskId "$worker_task" '
  [.[] | select(.taskId == $taskId and .schema == "woventeam.task_package.v0.1" and .parentTaskId != null and .requestedByRole == "project_manager")] | length == 1
' <<<"$tasks_json" >/dev/null
jq -e --arg taskId "$review_task" '
  [.[] | select(.taskId == $taskId and .schema == "woventeam.task_event.v0.1" and .status == "complete")] | length == 1
' <<<"$tasks_json" >/dev/null
jq -e --arg taskId "$blocked_child" '
  [.[] | select(.taskId == $taskId and .schema == "woventeam.task_event.v0.1" and .status == "blocked")] | length == 1
' <<<"$tasks_json" >/dev/null

grep -q '"messageType":"task.request"' "$ROOM_LOG"
grep -q '"messageType":"task.assign"' "$ROOM_LOG"
grep -q '"messageType":"task.result"' "$ROOM_LOG"

echo "wt-manager-subtasks integration test passed"
