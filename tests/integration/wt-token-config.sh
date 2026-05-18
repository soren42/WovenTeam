#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((22000 + RANDOM % 1000))}"
ROOM_LOG="$TMPDIR/phase0-room.jsonl"
TASK_LEDGER="$TMPDIR/task-packages.jsonl"
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

cat >"$CONFIG" <<EOF_CONFIG
roomName=phase0
roomLogPath=$ROOM_LOG
taskLedgerPath=$TASK_LEDGER
httpBindAddress=127.0.0.1
httpPort=$PORT
contextMessageCount=20
agentPollMilliseconds=1000
adapterTimeoutSeconds=1800
adapterMaxOutputBytes=1048576
fsyncEachMessage=0
enableCodexAdapter=0
tokenTelemetryEnabled=1
tokenDailyBudget=1000000
tokenMonthlyBudget=30000000
tokenWarningPercent=75
tokenCostPerMillionCents=500
runtimeRootPath=$TMPDIR/runtime
claudeMode=stub
chatgptMode=stub
geminiMode=stub
claudeCommand=claude
gptCommand=codex
geminiCommand=gemini
EOF_CONFIG

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
curl -fsS "$ROOM_URL/api/health" >/dev/null

config_json="$(curl -fsS "$ROOM_URL/api/config")"
jq -e '.tokenDailyBudget == 1000000 and .tokenMonthlyBudget == 30000000 and .tokenWarningPercent == 75' <<<"$config_json" >/dev/null

jq -cn \
  '{tokenTelemetryEnabled:1, tokenDailyBudget:2000000, tokenMonthlyBudget:50000000, tokenWarningPercent:80, tokenCostPerMillionCents:1000}' |
  curl -fsS -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/config" >/dev/null

grep -q '^tokenDailyBudget=2000000$' "$CONFIG"
grep -q '^tokenMonthlyBudget=50000000$' "$CONFIG"
grep -q '^tokenWarningPercent=80$' "$CONFIG"
grep -q '^tokenCostPerMillionCents=1000$' "$CONFIG"

response="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" create \
    --title "Token telemetry fixture" \
    --body "Record a token allocation budget." \
    --role backend_dev \
    --agent chatgpt \
    --initiative init_token_config \
    --max-tokens 123456
)"
test "$(jq -r '.ok' <<<"$response")" = "true"

tokens_json="$(curl -fsS "$ROOM_URL/api/tokens")"
jq -e '
  .tokenDailyBudget == 2000000 and
  .tokenMonthlyBudget == 50000000 and
  .dayWindowAllocatedTokens >= 123456 and
  .monthWindowAllocatedTokens >= 123456 and
  .allTimePackages >= 1
' <<<"$tokens_json" >/dev/null

echo "wt-token-config integration test passed"
