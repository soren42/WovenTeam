#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((24000 + RANDOM % 1000))}"
CONFIG="$TMPDIR/woventeam.conf"
ROOM_LOG="$TMPDIR/data/phase0-room.jsonl"
TASK_LEDGER="$TMPDIR/data/task-packages.jsonl"
PROJECTION="$TMPDIR/data/task-projection.sqlite"
RUNTIME_ROOT="$TMPDIR/runtime/tasks"
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

mkdir -p "$TMPDIR/data" "$RUNTIME_ROOT"
cat >"$TMPDIR/fake-claude" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf 'Phase 1 fake Claude result\n'
printf 'args: %s\n' "$*"
printf '%s\n' "$*" | grep -q -- '--print'
printf '%s\n' "$*" | grep -q 'Task title: Phase 1 routed adapter task'
printf 'Phase 1 fake Claude stderr\n' >&2
SH
chmod +x "$TMPDIR/fake-claude"

cat >"$CONFIG" <<CFG
roomName=phase1_e2e
roomLogPath=$ROOM_LOG
taskLedgerPath=$TASK_LEDGER
taskProjectionDbPath=$PROJECTION
httpBindAddress=127.0.0.1
httpPort=$PORT
contextMessageCount=20
agentPollMilliseconds=1000
adapterTimeoutSeconds=10
adapterMaxOutputBytes=1048576
maxActiveTasksPerAgent=12
maxSubtasksPerParent=10
maxTasksPerInitiative=32
fsyncEachMessage=0
roleRoutingEnabled=1
enableCodexAdapter=0
enableClaudeAdapter=1
enableGeminiAdapter=0
tokenTelemetryEnabled=1
tokenDailyBudget=10000000
tokenMonthlyBudget=10000000
tokenWarningPercent=80
tokenCostPerMillionCents=1000
runtimeRootPath=$RUNTIME_ROOT
claudeMode=adapter
chatgptMode=stub
geminiMode=stub
claudeCommand=$TMPDIR/fake-claude
gptCommand=codex
geminiCommand=gemini
CFG

start_roomd() {
  "$ROOT/build/wt-roomd" --config "$CONFIG" >"$SERVER_LOG" 2>&1 &
  SERVER_PID=$!
  for _ in $(seq 1 50); do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
      cat "$SERVER_LOG" >&2 || true
      echo "wt-roomd exited before health check" >&2
      exit 1
    fi
    if curl -fsS "$ROOM_URL/api/health" >/dev/null 2>&1; then
      return
    fi
    sleep 0.1
  done
  curl -fsS "$ROOM_URL/api/health" >/dev/null
}

restart_roomd() {
  kill "$SERVER_PID" 2>/dev/null || true
  wait "$SERVER_PID" 2>/dev/null || true
  SERVER_PID=""
  rm -f "$PROJECTION" "$PROJECTION-shm" "$PROJECTION-wal"
  start_roomd
}

post_event() {
  local task_id="$1" event_type="$2" status="$3" message="$4"
  curl -fsS -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/task-event" >/dev/null <<JSON
{"schema":"woventeam.task_event.v0.1","taskId":"$task_id","eventType":"$event_type","status":"$status","message":"$message","createdBy":"phase1-e2e","createdAtUnixMs":1778915000000}
JSON
}

start_roomd

curl -fsS "$ROOM_URL/api/health" |
  jq -e '.ok == true and .projection.ok == true' >/dev/null
curl -fsS "$ROOM_URL/api/adapters" |
  jq -e '.adapters[] | select(.agent == "claude" and .enabled == true and .mode == "adapter" and .state == "launchable")' >/dev/null

initiative="init_phase1_e2e"
parent_response="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" create \
    --title "Phase 1 initiative manager" \
    --body "Create and route an adapter-backed task for final Phase 1 validation." \
    --role project_manager \
    --agent router \
    --initiative "$initiative" \
    --tool-profile observe \
    --max-tokens 1000
)"
parent_id="$(jq -r '.taskId' <<<"$parent_response")"
test "$parent_id" != "null"
curl -fsS "$ROOM_URL/api/task-detail?taskId=$parent_id" |
  jq -e '.task.assignedAgent == "claude" and .task.status == "assigned"' >/dev/null

child_response="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" request \
    --parent "$parent_id" \
    --by-role project_manager \
    --by claude \
    --role technical_writer \
    --title "Phase 1 routed adapter task" \
    --body "Produce the final Phase 1 proof artifact." \
    --agent router \
    --initiative "$initiative" \
    --tool-profile observe \
    --max-tokens 2000
)"
child_id="$(jq -r '.taskId' <<<"$child_response")"
test "$child_id" != "null"
curl -fsS "$ROOM_URL/api/task-detail?taskId=$child_id" |
  jq -e '.task.parentTaskId == "'"$parent_id"'" and .task.assignedAgent == "claude" and .task.status == "queued"' >/dev/null
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" update-status "$parent_id" --status complete --message "manager accepted routed child" >/dev/null

WT_ROOM_LOG_PATH="$ROOM_LOG" \
WT_TASK_LEDGER_PATH="$TASK_LEDGER" \
WT_TASK_PROJECTION_DB_PATH="$PROJECTION" \
WT_RUNTIME_ROOT_PATH="$RUNTIME_ROOT" \
WT_ENABLE_CLAUDE_ADAPTER=1 \
WT_CLAUDE_MODE=adapter \
WT_CLAUDE_COMMAND="$TMPDIR/fake-claude" \
  "$ROOT/build/wt-agent" --agent claude --once >"$TMPDIR/agent.out"

curl -fsS "$ROOM_URL/api/task-detail?taskId=$child_id" |
  jq -e '
    .task.status == "complete" and
    ([.events[] | select(.status == "running")] | length) == 1 and
    ([.events[] | select(.status == "complete")] | length) == 1
  ' >/dev/null

curl -fsS "$ROOM_URL/api/task-artifacts?taskId=$child_id" |
  jq -e '
    .exists == true and
    (.files | map(.name) | index("prompt.md")) and
    (.files | map(.name) | index("result.md")) and
    (.files | map(.name) | index("stdout.log")) and
    (.files | map(.name) | index("stderr.log")) and
    (.files | map(.name) | index("manifest.json")) and
    (.resultText | contains("Phase 1 fake Claude result")) and
    (.stdoutText | contains("Phase 1 fake Claude result")) and
    (.stderrText | contains("Phase 1 fake Claude stderr")) and
    (.manifestText | contains("\"adapter\": \"claude\""))
  ' >/dev/null

curl -fsS -H 'Content-Type: application/json' \
  -d '{"taskId":"'"$child_id"'","provider":"anthropic","modelId":"anthropic/claude-sonnet","inputTokens":111,"outputTokens":222,"estimatedCostCents":9,"createdBy":"wt-agent@claude"}' \
  "$ROOM_URL/api/task-usage" >/dev/null
curl -fsS "$ROOM_URL/api/tokens" |
  jq -e '.dayWindowAllocatedTokens == 3000 and .dayWindowActualTokens == 333 and .dayWindowActualCostCents == 9 and .allTimeUsageEvents == 1' >/dev/null

for action in approve reject revision; do
  curl -fsS -H 'Content-Type: application/json' \
    -d '{"taskId":"'"$child_id"'","action":"'"$action"'","createdBy":"ceo"}' \
    "$ROOM_URL/api/task-gate" >/dev/null
done
curl -fsS "$ROOM_URL/api/task-detail?taskId=$child_id" |
  jq -e '
    ([.events[] | select(.eventType == "review_gate" and .status == "approved")] | length) == 1 and
    ([.events[] | select(.eventType == "review_gate" and .status == "rejected")] | length) == 1 and
    ([.events[] | select(.eventType == "review_gate" and .status == "revision_requested")] | length) == 1
  ' >/dev/null

failed_id="task_phase1_failed"
blocked_id="task_phase1_blocked"
retry_id="task_phase1_retry"
closed_id="task_phase1_closed"
for task_id in "$failed_id" "$blocked_id" "$retry_id" "$closed_id"; do
  curl -fsS -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/task-package" >/dev/null <<JSON
{"schema":"woventeam.task_package.v0.1","taskId":"$task_id","initiativeId":"$initiative","createdBy":"phase1-e2e","assignedRole":"tester","assignedAgent":"chatgpt","modelId":"openai/gpt-5.3-codex","priority":"normal","status":"queued","title":"$task_id","body":"Lifecycle coverage fixture.","task":{"title":"$task_id","body":"Lifecycle coverage fixture.","deliverables":[]},"contextRefs":[],"acceptanceCriteria":[],"toolPolicy":{"profile":"observe","filesystem":"read_only","network":"none","system":"none","git":"none"},"budget":{"timeoutSeconds":1800,"maxOutputBytes":1048576,"maxCostUsd":1.0,"maxTokens":10},"dependencies":[],"createdAtUnixMs":1778915000000}
JSON
done
post_event "$failed_id" "status" "failed" "failure state recorded"
post_event "$blocked_id" "status" "blocked" "blocked state recorded"
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" retry "$retry_id" --message "retry state recorded" >/dev/null
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" close "$closed_id" --message "closed state recorded" >/dev/null
curl -fsS "$ROOM_URL/api/task-summaries" |
  jq -e '
    ([.[] | select(.taskId == "'"$failed_id"'" and .status == "failed")] | length) == 1 and
    ([.[] | select(.taskId == "'"$blocked_id"'" and .status == "blocked")] | length) == 1 and
    ([.[] | select(.taskId == "'"$retry_id"'" and .status == "queued")] | length) == 1 and
    ([.[] | select(.taskId == "'"$closed_id"'" and .status == "closed")] | length) == 1
  ' >/dev/null

curl -fsS "$ROOM_URL/api/capacity" |
  jq -e '.caps.maxActiveTasksPerAgent == 12 and ([.agents[] | select(.agent == "claude")] | length) >= 1' >/dev/null

WT_ROOT="$TMPDIR" WT_BACKUP_ROOT="$TMPDIR/backups" WT_BACKUP_STAMP="phase1e2e" "$ROOT/bin/wt-ops-backup" --dry-run |
  grep -q "include data/task-packages.jsonl"
WT_ROOT="$TMPDIR" WT_BACKUP_ROOT="$TMPDIR/backups" WT_BACKUP_STAMP="phase1e2e" "$ROOT/bin/wt-ops-backup" >/dev/null
test -f "$TMPDIR/backups/woventeam-phase1e2e/data/task-packages.jsonl"
test -f "$TMPDIR/backups/woventeam-phase1e2e/runtime/tasks/$child_id/result.md"
WT_ROOT="$TMPDIR" WT_BACKUP_ROOT="$TMPDIR/backups" "$ROOT/bin/wt-ops-restore" --latest --dry-run |
  grep -q "restore data/task-packages.jsonl"

restart_roomd
curl -fsS "$ROOM_URL/api/task-detail?taskId=$child_id" |
  jq -e '.task.status == "revision_requested" and ([.events[] | select(.status == "complete")] | length) == 1' >/dev/null
curl -fsS "$ROOM_URL/api/task-artifacts?taskId=$child_id" |
  jq -e '.exists == true and (.resultText | contains("Phase 1 fake Claude result"))' >/dev/null
curl -fsS "$ROOM_URL/api/tokens" |
  jq -e '.dayWindowAllocatedTokens == 3000 and .allTimeAllocatedTokens >= 3040 and .dayWindowActualTokens == 333' >/dev/null

echo "wt-phase1-e2e integration test passed"
