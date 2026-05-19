#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((26000 + RANDOM % 1000))}"
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
roomName=initiative_test
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

initiative="init_phase2_spike2"
parent_response="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" initiative create \
    --id "$initiative" \
    --title "Phase 2 initiative charter" \
    --body "Validate initiative summary and detail paths." \
    --max-tokens 1200
)"
parent_id="$(jq -r '.taskId' <<<"$parent_response")"
test "$parent_id" != "null"

child_response="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" request \
    --parent "$parent_id" \
    --by-role project_manager \
    --by claude \
    --role backend_dev \
    --title "Initiative child task" \
    --body "Validate initiative child projection." \
    --agent router \
    --initiative "$initiative" \
    --tool-profile repo_branch \
    --max-tokens 800
)"
child_id="$(jq -r '.taskId' <<<"$child_response")"
test "$child_id" != "null"

WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" update-status "$child_id" --status blocked --message "blocked for initiative coverage" >/dev/null

curl -fsS "$ROOM_URL/api/initiatives" |
  jq -e '
    .ok == true and
    ([.initiatives[] | select(.initiativeId == "'"$initiative"'" and .taskCount == 2 and .activeTasks == 2 and .blockedTasks == 1 and .maxTokens == 2000)] | length) == 1
  ' >/dev/null

curl -fsS "$ROOM_URL/api/initiative-detail?initiativeId=$initiative" |
  jq -e '
    .ok == true and
    .initiative.initiativeId == "'"$initiative"'" and
    .initiative.taskCount == 2 and
    ([.tasks[] | select(.taskId == "'"$parent_id"'")] | length) == 1 and
    ([.tasks[] | select(.taskId == "'"$child_id"'" and .status == "blocked")] | length) == 1
  ' >/dev/null

WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" initiative list | grep -q "$initiative"
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" initiative show "$initiative" |
  jq -e '.ok == true and .initiative.taskCount == 2' >/dev/null
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" initiative export "$initiative" |
  jq -e '.ok == true and (.tasks | length) == 2' >/dev/null

WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" initiative close "$initiative" --message "initiative close coverage" |
  jq -e '.ok == true and .initiative.activeTasks == 0 and ([.tasks[] | select(.status == "closed")] | length) == 2' >/dev/null

echo "wt-initiatives integration test passed"
