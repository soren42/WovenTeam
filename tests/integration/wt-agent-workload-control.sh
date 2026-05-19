#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((27000 + RANDOM % 1000))}"
CONFIG="$TMPDIR/woventeam.conf"
ROOM_LOG="$TMPDIR/data/phase0-room.jsonl"
TASK_LEDGER="$TMPDIR/data/task-packages.jsonl"
PROJECTION="$TMPDIR/data/task-projection.sqlite"
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

mkdir -p "$TMPDIR/data" "$TMPDIR/runtime/tasks"
cat >"$CONFIG" <<CFG
roomName=workload_control
roomLogPath=$ROOM_LOG
taskLedgerPath=$TASK_LEDGER
taskProjectionDbPath=$PROJECTION
httpBindAddress=127.0.0.1
httpPort=$PORT
contextMessageCount=20
agentPollMilliseconds=1000
adapterTimeoutSeconds=1800
adapterMaxOutputBytes=1048576
maxActiveTasksPerAgent=8
maxSubtasksPerParent=8
maxTasksPerInitiative=20
fsyncEachMessage=0
roleRoutingEnabled=1
enableCodexAdapter=0
enableClaudeAdapter=0
enableGeminiAdapter=0
tokenTelemetryEnabled=1
tokenDailyBudget=10000000
tokenMonthlyBudget=10000000
tokenWarningPercent=80
tokenCostPerMillionCents=1000
runtimeRootPath=$TMPDIR/runtime/tasks
claudeMode=stub
chatgptMode=stub
geminiMode=stub
claudeCommand=claude
gptCommand=codex
geminiCommand=gemini
CFG

"$ROOT/build/wt-roomd" --config "$CONFIG" >"$SERVER_LOG" 2>&1 &
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

task_response="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" create \
    --title "Workload lease task" \
    --body "Validate pause, resume, lease, and attempts." \
    --role backend_dev \
    --agent chatgpt \
    --initiative init_workload_control \
    --max-tokens 100
)"
task_id="$(jq -r '.taskId' <<<"$task_response")"
test "$task_id" != "null"

WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" agent pause chatgpt --message "pause coverage" >/dev/null
WT_ROOM_LOG_PATH="$ROOM_LOG" WT_TASK_LEDGER_PATH="$TASK_LEDGER" "$ROOT/build/wt-agent" --agent chatgpt --once >/dev/null
curl -fsS "$ROOM_URL/api/task-detail?taskId=$task_id" |
  jq -e '.task.status == "assigned" and ([.events[] | select(.eventType == "lease")] | length) == 0' >/dev/null
curl -fsS "$ROOM_URL/api/agents" |
  jq -e '(.agents[] | select(.agent == "chatgpt")).state == "paused"' >/dev/null

WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" agent resume chatgpt --message "resume coverage" >/dev/null
WT_ROOM_LOG_PATH="$ROOM_LOG" WT_TASK_LEDGER_PATH="$TASK_LEDGER" "$ROOT/build/wt-agent" --agent chatgpt --once >/dev/null
curl -fsS "$ROOM_URL/api/task-detail?taskId=$task_id" |
  jq -e '
    .task.status == "complete" and
    .task.attemptCount == 1 and
    ([.events[] | select(.eventType == "lease" and .status == "leased")] | length) == 1 and
    ([.events[] | select(.status == "running")] | length) == 1 and
    ([.events[] | select(.status == "complete")] | length) == 1
  ' >/dev/null
curl -fsS "$ROOM_URL/api/agents" |
  jq -e '(.agents[] | select(.agent == "chatgpt")).state == "active" and (.agents[] | select(.agent == "chatgpt")).attempts == 1' >/dev/null

old_ms="$(( ($(date +%s) - 3600) * 1000 ))"
curl -fsS -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/task-package" >/dev/null <<JSON
{"schema":"woventeam.task_package.v0.1","taskId":"task_stuck_lease","initiativeId":"init_workload_control","createdBy":"test","assignedRole":"technical_writer","assignedAgent":"gemini","modelId":"google/gemini-pro","priority":"normal","status":"queued","title":"Stuck lease fixture","body":"fixture","task":{"title":"Stuck lease fixture","body":"fixture","deliverables":[]},"contextRefs":[],"acceptanceCriteria":[],"toolPolicy":{"profile":"observe"},"budget":{"maxTokens":1},"dependencies":[],"createdAtUnixMs":$old_ms}
JSON
curl -fsS -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/task-event" >/dev/null <<JSON
{"schema":"woventeam.task_event.v0.1","taskId":"task_stuck_lease","eventType":"lease","status":"leased","assignedAgent":"gemini","message":"old lease","createdBy":"wt-agent@gemini","attempt":1,"leaseExpiresAtUnixMs":$old_ms,"createdAtUnixMs":$old_ms}
JSON
curl -fsS "$ROOM_URL/api/agents" |
  jq -e '(.agents[] | select(.agent == "gemini")).stuckTasks == 1' >/dev/null

echo "wt-agent-workload-control integration test passed"
