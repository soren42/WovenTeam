#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((22000 + RANDOM % 1000))}"
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
roomName=quota_ops
roomLogPath=$ROOM_LOG
taskLedgerPath=$TASK_LEDGER
taskProjectionDbPath=$PROJECTION
httpBindAddress=127.0.0.1
httpPort=$PORT
contextMessageCount=20
agentPollMilliseconds=1000
adapterTimeoutSeconds=1800
adapterMaxOutputBytes=1048576
maxActiveTasksPerAgent=4
maxSubtasksPerParent=8
maxTasksPerInitiative=32
fsyncEachMessage=0
roleRoutingEnabled=1
enableCodexAdapter=0
enableClaudeAdapter=0
enableGeminiAdapter=0
tokenTelemetryEnabled=1
tokenDailyBudget=1000
tokenMonthlyBudget=2000
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

WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" create \
  --title "Quota allowed" \
  --body "Allowed within budget." \
  --role backend_dev \
  --agent chatgpt \
  --initiative init_quota_ops \
  --max-tokens 600 >/dev/null

blocked_status="$(
  curl -sS -o "$TMPDIR/quota-response.json" -w '%{http_code}' \
    -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/task-package" <<JSON
{"schema":"woventeam.task_package.v0.1","taskId":"task_quota_blocked","initiativeId":"init_quota_ops","createdBy":"ceo","assignedRole":"backend_dev","assignedAgent":"chatgpt","modelId":"openai/gpt-5.3-codex","priority":"normal","status":"queued","title":"Quota blocked","body":"Should exceed daily budget.","task":{"title":"Quota blocked","body":"Should exceed daily budget.","deliverables":[]},"contextRefs":[],"acceptanceCriteria":[],"toolPolicy":{"profile":"observe","filesystem":"read_only","network":"none","system":"none","git":"none"},"budget":{"timeoutSeconds":1800,"maxOutputBytes":1048576,"maxCostUsd":1.0,"maxTokens":500},"dependencies":[],"createdAtUnixMs":1778915000000}
JSON
)"
test "$blocked_status" = "409"
grep -q "daily token budget exceeded" "$TMPDIR/quota-response.json"

curl -fsS -H 'Content-Type: application/json' \
  -d '{"taskId":"task_usage_1","provider":"openai","modelId":"openai/gpt-5.3-codex","inputTokens":100,"outputTokens":250,"estimatedCostCents":7,"createdBy":"test"}' \
  "$ROOM_URL/api/task-usage" >/dev/null

curl -fsS "$ROOM_URL/api/tokens" |
  jq -e '.dayWindowAllocatedTokens == 600 and .dayWindowActualTokens == 350 and .dayWindowActualCostCents == 7 and .allTimeUsageEvents == 1' >/dev/null

curl -fsS "$ROOM_URL/api/health" |
  jq -e '.ok == true and .ledger.exists == true and .projection.ok == true and .roomLog.exists == true' >/dev/null

echo "room event" > "$ROOM_LOG"
echo '{"schema":"woventeam.task_package.v0.1","taskId":"backup_task"}' > "$TASK_LEDGER"
echo "agent state" > "$TMPDIR/data/chatgpt.state"
echo "runtime artifact" > "$TMPDIR/runtime/tasks/result.md"

WT_ROOT="$TMPDIR" WT_BACKUP_ROOT="$TMPDIR/backups" WT_BACKUP_STAMP="teststamp" "$ROOT/bin/wt-ops-backup" --dry-run | grep -q "include data/task-packages.jsonl"
WT_ROOT="$TMPDIR" WT_BACKUP_ROOT="$TMPDIR/backups" WT_BACKUP_STAMP="teststamp" "$ROOT/bin/wt-ops-backup" >/dev/null
test -f "$TMPDIR/backups/woventeam-teststamp/data/task-packages.jsonl"
test -f "$TMPDIR/backups/woventeam-teststamp/runtime/tasks/result.md"
WT_ROOT="$TMPDIR" WT_BACKUP_ROOT="$TMPDIR/backups" "$ROOT/bin/wt-ops-restore" --latest --dry-run | grep -q "restore data/task-packages.jsonl"

echo "wt-quotas-ops integration test passed"
