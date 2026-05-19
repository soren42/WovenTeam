#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((21000 + RANDOM % 1000))}"
ROOM_LOG="$TMPDIR/phase0-room.jsonl"
TASK_LEDGER="$TMPDIR/task-packages.jsonl"
PROJECTION="$TMPDIR/task-projection.sqlite"
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

WT_ROOM_LOG_PATH="$ROOM_LOG" \
WT_TASK_LEDGER_PATH="$TASK_LEDGER" \
WT_TASK_PROJECTION_DB_PATH="$PROJECTION" \
WT_MAX_ACTIVE_TASKS_PER_AGENT=2 \
WT_MAX_SUBTASKS_PER_PARENT=1 \
WT_MAX_TASKS_PER_INITIATIVE=3 \
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

parent_id="task_route_parent"
initiative="init_routing_gate"
curl -fsS -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/task-package" >/dev/null <<JSON
{"schema":"woventeam.task_package.v0.1","taskId":"$parent_id","initiativeId":"$initiative","createdBy":"ceo","assignedRole":"project_manager","assignedAgent":"router","modelId":"anthropic/claude-sonnet","priority":"normal","status":"queued","title":"Routing parent","body":"Parent task for routing gate smoke.","task":{"title":"Routing parent","body":"Parent task for routing gate smoke.","deliverables":[]},"contextRefs":[],"acceptanceCriteria":[],"toolPolicy":{"profile":"observe","filesystem":"read_only","network":"none","system":"none","git":"none"},"budget":{"timeoutSeconds":1800,"maxOutputBytes":1048576,"maxCostUsd":1.0,"maxTokens":1000},"dependencies":[],"createdAtUnixMs":1778915000000}
JSON

curl -fsS "$ROOM_URL/api/task-detail?taskId=$parent_id" |
  jq -e '.task.assignedAgent == "claude" and .task.status == "assigned"' >/dev/null

child_id="task_route_child"
curl -fsS -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/task-request" >/dev/null <<JSON
{"schema":"woventeam.task_request.v0.1","taskId":"$child_id","parentTaskId":"$parent_id","requestedTaskId":"$child_id","initiativeId":"$initiative","requestedBy":"project_manager","requestedByRole":"project_manager","requestedRole":"backend_dev","assignedAgent":"router","modelId":"openai/gpt-5.3-codex","priority":"normal","title":"Routed backend child","body":"Child task for capacity check.","toolPolicy":{"profile":"repo_branch"},"budget":{"maxTokens":1000},"createdAtUnixMs":1778915001000}
JSON

curl -fsS "$ROOM_URL/api/task-detail?taskId=$child_id" |
  jq -e '.task.assignedAgent == "chatgpt" and .task.toolProfile == "repo_branch"' >/dev/null

blocked_status="$(
  curl -sS -o "$TMPDIR/capacity-response.json" -w '%{http_code}' \
    -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/task-request" <<JSON
{"schema":"woventeam.task_request.v0.1","taskId":"task_route_child_2","parentTaskId":"$parent_id","requestedTaskId":"task_route_child_2","initiativeId":"$initiative","requestedBy":"project_manager","requestedByRole":"project_manager","requestedRole":"tester","assignedAgent":"router","modelId":"openai/gpt-5.3-codex","priority":"normal","title":"Blocked child","body":"Should be blocked by parent cap.","toolPolicy":{"profile":"repo_branch"},"budget":{"maxTokens":1000},"createdAtUnixMs":1778915002000}
JSON
)"
test "$blocked_status" = "409"
grep -q "parent subtask capacity exceeded" "$TMPDIR/capacity-response.json"

curl -fsS "$ROOM_URL/api/capacity" |
  jq -e '.caps.maxSubtasksPerParent == 1 and ([.agents[] | select(.agent == "chatgpt" and .activeTasks == 1)] | length) == 1' >/dev/null

curl -fsS -H 'Content-Type: application/json' \
  -d '{"taskId":"task_route_child","action":"approve","createdBy":"ceo"}' \
  "$ROOM_URL/api/task-gate" >/dev/null
curl -fsS "$ROOM_URL/api/task-detail?taskId=$child_id" |
  jq -e '([.events[] | select(.eventType == "review_gate" and .status == "approved")] | length) == 1' >/dev/null

echo "wt-routing-gates integration test passed"
