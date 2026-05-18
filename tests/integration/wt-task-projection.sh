#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((23000 + RANDOM % 1000))}"
ROOM_LOG="$TMPDIR/phase0-room.jsonl"
TASK_LEDGER="$TMPDIR/task-packages.jsonl"
TASK_DB="$TMPDIR/task-projection.sqlite"
CONFIG="$TMPDIR/woventeam-phase0.conf"
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

write_config() {
    cat >"$CONFIG" <<EOF_CONFIG
roomName=phase0
roomLogPath=$ROOM_LOG
taskLedgerPath=$TASK_LEDGER
taskProjectionDbPath=$TASK_DB
httpBindAddress=127.0.0.1
httpPort=$PORT
contextMessageCount=20
agentPollMilliseconds=1000
adapterTimeoutSeconds=1800
adapterMaxOutputBytes=1048576
fsyncEachMessage=0
enableCodexAdapter=0
tokenTelemetryEnabled=1
tokenDailyBudget=2000000
tokenMonthlyBudget=50000000
tokenWarningPercent=80
tokenCostPerMillionCents=1000
runtimeRootPath=$TMPDIR/runtime
claudeMode=stub
chatgptMode=stub
geminiMode=stub
claudeCommand=claude
gptCommand=codex
geminiCommand=gemini
EOF_CONFIG
}

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
    rm -f "$TASK_DB" "$TASK_DB-shm" "$TASK_DB-wal"
    start_roomd
}

write_config
start_roomd

response="$(
    WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" create \
        --title "Projection fixture" \
        --body "Verify SQLite projection and task detail APIs." \
        --role backend_dev \
        --agent chatgpt \
        --initiative init_projection \
        --max-tokens 654321
)"
task_id="$(jq -r '.taskId' <<<"$response")"
test "$task_id" != "null"

WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" update-status "$task_id" --status running --message "projection running" >/dev/null
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" close "$task_id" --message "projection closed" >/dev/null

summaries="$(curl -fsS "$ROOM_URL/api/task-summaries")"
jq -e --arg taskId "$task_id" '
  [.[] | select(.taskId == $taskId and .status == "closed" and .maxTokens == 654321 and .eventCount >= 2)] | length == 1
' <<<"$summaries" >/dev/null

detail="$(curl -fsS "$ROOM_URL/api/task-detail?taskId=$task_id")"
jq -e --arg taskId "$task_id" '
  .ok == true and
  .task.taskId == $taskId and
  .task.status == "closed" and
  ([.events[] | select(.status == "running")] | length) == 1 and
  ([.events[] | select(.status == "closed")] | length) == 1
' <<<"$detail" >/dev/null

WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" show "$task_id" | jq -e '.ok == true and .task.status == "closed"' >/dev/null
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" list | grep -q "$task_id"

sqlite3 "$TASK_DB" "SELECT status FROM tasks WHERE task_id='$task_id';" | grep -q '^closed$'

restart_roomd

curl -fsS "$ROOM_URL/api/task-detail?taskId=$task_id" |
  jq -e '.ok == true and .task.status == "closed" and (.events | length) >= 2' >/dev/null

echo "wt-task-projection integration test passed"
