#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((28000 + RANDOM % 1000))}"
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
cat >"$TMPDIR/fake-codex" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >> "$WT_FAKE_CODEX_ARGS"
out=""
workspace=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --output-last-message) out="$2"; shift 2 ;;
    --cd) workspace="$2"; shift 2 ;;
    *) shift ;;
  esac
done
test -n "$out"
test -n "$workspace"
printf 'autonomy result\n' > "$out"
SH
chmod +x "$TMPDIR/fake-codex"

cat >"$CONFIG" <<CFG
roomName=autonomy
roomLogPath=$ROOM_LOG
taskLedgerPath=$TASK_LEDGER
taskProjectionDbPath=$PROJECTION
httpBindAddress=127.0.0.1
httpPort=$PORT
contextMessageCount=20
agentPollMilliseconds=1000
adapterTimeoutSeconds=30
adapterMaxOutputBytes=1048576
maxActiveTasksPerAgent=8
maxSubtasksPerParent=8
maxTasksPerInitiative=20
fsyncEachMessage=0
roleRoutingEnabled=1
enableCodexAdapter=1
enableClaudeAdapter=0
enableGeminiAdapter=0
tokenTelemetryEnabled=1
tokenDailyBudget=10000000
tokenMonthlyBudget=10000000
tokenWarningPercent=80
tokenCostPerMillionCents=1000
runtimeRootPath=$RUNTIME_ROOT
claudeMode=stub
chatgptMode=adapter
geminiMode=stub
claudeCommand=claude
gptCommand=$TMPDIR/fake-codex
geminiCommand=gemini
CFG

"$ROOT/build/wt-roomd" --config "$CONFIG" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 50); do
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    cat "$SERVER_LOG" >&2 || true
    exit 1
  fi
  if curl -fsS "$ROOM_URL/api/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" create \
  --title "Autonomous adapter smoke" \
  --body "Run fake Codex with elevated autonomy." \
  --role backend_dev \
  --agent chatgpt \
  --initiative init_autonomy \
  --model openai/gpt-5.3-codex \
  --tool-profile repo_branch \
  --max-tokens 100 \
  --autonomy-level autonomous \
  --autonomy-ttl 3600 \
  --autonomy-scope workspace >/tmp/wt-autonomy-create.json
task_id="$(jq -r '.taskId' /tmp/wt-autonomy-create.json)"
test "$task_id" != "null"

WT_ROOM_LOG_PATH="$ROOM_LOG" WT_TASK_LEDGER_PATH="$TASK_LEDGER" \
WT_RUNTIME_ROOT_PATH="$RUNTIME_ROOT" WT_ENABLE_CODEX_ADAPTER=1 \
WT_CHATGPT_MODE=adapter WT_GPT_COMMAND="$TMPDIR/fake-codex" \
WT_FAKE_CODEX_ARGS="$TMPDIR/fake-codex.args" \
  "$ROOT/build/wt-agent" --agent chatgpt --once >/dev/null

# Elevated (granted) invocation: codex runs fully non-interactive via the
# single bypass flag. The installed codex CLI has no --ask-for-approval flag,
# and the bypass switch replaces --sandbox danger-full-access.
grep -q -- '--dangerously-bypass-approvals-and-sandbox' "$TMPDIR/fake-codex.args"
! grep -q -- '--ask-for-approval' "$TMPDIR/fake-codex.args"
jq -s -e --arg taskId "$task_id" '
  ([.[] | select(.schema == "woventeam.autonomy_event.v0.1" and .taskId == $taskId and .action == "adapter_invocation_elevated")] | length) == 1 and
  ([.[] | select(.schema == "woventeam.autonomy_event.v0.1" and .taskId == $taskId and .action == "adapter_exit_elevated" and .exitCode == 0)] | length) == 1
' "$TASK_LEDGER" >/dev/null

expired_ms="$(( ($(date +%s) - 10) * 1000 ))"
expired_code="$(
  curl -sS -o "$TMPDIR/expired.json" -w '%{http_code}' -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/task-package" <<JSON
{"schema":"woventeam.task_package.v0.1","taskId":"task_expired_autonomy","initiativeId":"init_autonomy","createdBy":"ceo","assignedRole":"backend_dev","assignedAgent":"chatgpt","modelId":"openai/gpt-5.3-codex","priority":"normal","status":"queued","title":"expired","body":"expired","toolPolicy":{"profile":"repo_branch"},"budget":{"maxTokens":1},"autonomyLevel":"autonomous","autonomyGrant":{"scope":"workspace","ttlSeconds":1,"network":"none","credentialClass":"none"},"createdAtUnixMs":$expired_ms}
JSON
)"
test "$expired_code" = "409"
jq -e '.reason == "autonomy_expired"' "$TMPDIR/expired.json" >/dev/null

missing_code="$(
  curl -sS -o "$TMPDIR/missing.json" -w '%{http_code}' -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/task-package" <<JSON
{"schema":"woventeam.task_package.v0.1","taskId":"task_missing_autonomy","initiativeId":"init_autonomy","createdBy":"ceo","assignedRole":"backend_dev","assignedAgent":"chatgpt","modelId":"openai/gpt-5.3-codex","priority":"normal","status":"queued","title":"missing","body":"missing","toolPolicy":{"profile":"repo_branch"},"budget":{"maxTokens":1},"autonomyLevel":"autonomous","createdAtUnixMs":$(date +%s%3N)}
JSON
)"
test "$missing_code" = "409"
jq -e '.reason == "autonomy_required"' "$TMPDIR/missing.json" >/dev/null

WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" create \
  --title "Revoked autonomy adapter smoke" \
  --body "Run fake Codex after revocation; it must downgrade flags." \
  --role backend_dev \
  --agent chatgpt \
  --initiative init_autonomy \
  --model openai/gpt-5.3-codex \
  --tool-profile repo_branch \
  --max-tokens 100 \
  --autonomy-level autonomous \
  --autonomy-ttl 3600 \
  --autonomy-scope workspace >/tmp/wt-autonomy-revoked-create.json
revoked_task_id="$(jq -r '.taskId' /tmp/wt-autonomy-revoked-create.json)"
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" autonomy revoke "$revoked_task_id" --reason "test revoke" >/dev/null
: > "$TMPDIR/fake-codex.args"
WT_ROOM_LOG_PATH="$ROOM_LOG" WT_TASK_LEDGER_PATH="$TASK_LEDGER" \
WT_RUNTIME_ROOT_PATH="$RUNTIME_ROOT" WT_ENABLE_CODEX_ADAPTER=1 \
WT_CHATGPT_MODE=adapter WT_GPT_COMMAND="$TMPDIR/fake-codex" \
WT_FAKE_CODEX_ARGS="$TMPDIR/fake-codex.args" \
  "$ROOT/build/wt-agent" --agent chatgpt --once >/dev/null

# Non-elevated (revoked / ungranted) invocation: codex runs in the
# workspace-write sandbox and must NOT receive the bypass switch.
grep -q -- '--sandbox workspace-write' "$TMPDIR/fake-codex.args"
! grep -q -- '--dangerously-bypass-approvals-and-sandbox' "$TMPDIR/fake-codex.args"
jq -s -e --arg taskId "$revoked_task_id" '
  ([.[] | select(.schema == "woventeam.autonomy_event.v0.1" and .taskId == $taskId and .action == "revoked")] | length) == 1 and
  ([.[] | select(.schema == "woventeam.kill_event.v0.1" and .taskId == $taskId and .reason == "autonomy-revoke")] | length) == 1 and
  ([.[] | select(.schema == "woventeam.autonomy_event.v0.1" and .taskId == $taskId and .action == "adapter_invocation" and .allowed == false)] | length) == 1
' "$TASK_LEDGER" >/dev/null

echo "wt-autonomy integration test passed"
