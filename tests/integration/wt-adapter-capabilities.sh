#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((24000 + RANDOM % 1000))}"
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

cat >"$TMPDIR/fake-codex" <<'SH'
#!/usr/bin/env bash
echo fake codex
SH
cat >"$TMPDIR/fake-claude" <<'SH'
#!/usr/bin/env bash
echo fake claude
SH
cat >"$TMPDIR/fake-gemini" <<'SH'
#!/usr/bin/env bash
echo fake gemini
SH
chmod +x "$TMPDIR/fake-codex" "$TMPDIR/fake-claude" "$TMPDIR/fake-gemini"

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
enableClaudeAdapter=0
enableGeminiAdapter=0
tokenTelemetryEnabled=1
tokenDailyBudget=2000000
tokenMonthlyBudget=50000000
tokenWarningPercent=80
tokenCostPerMillionCents=1000
runtimeRootPath=$TMPDIR/runtime
claudeMode=stub
chatgptMode=stub
geminiMode=stub
claudeCommand=$TMPDIR/fake-claude
gptCommand=$TMPDIR/fake-codex
geminiCommand=$TMPDIR/fake-gemini
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

curl -fsS "$ROOM_URL/api/adapters" |
  jq -e '
    .ok == true and
    ([.adapters[] | select(.state == "launchable")] | length) == 3 and
    ([.adapters[] | select(.enabled == false)] | length) == 3
  ' >/dev/null

jq -cn \
  --arg claude "$TMPDIR/fake-claude" \
  --arg codex "$TMPDIR/fake-codex" \
  --arg gemini "$TMPDIR/fake-gemini" \
  '{
    tokenTelemetryEnabled:1,
    tokenDailyBudget:2000000,
    tokenMonthlyBudget:50000000,
    tokenWarningPercent:80,
    tokenCostPerMillionCents:1000,
    enableCodexAdapter:1,
    enableClaudeAdapter:1,
    enableGeminiAdapter:1,
    claudeMode:"adapter",
    chatgptMode:"stub",
    geminiMode:"adapter",
    claudeCommand:$claude,
    gptCommand:$codex,
    geminiCommand:$gemini
  }' |
  curl -fsS -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/config" >/dev/null

grep -q '^enableClaudeAdapter=1$' "$CONFIG"
grep -q '^enableGeminiAdapter=1$' "$CONFIG"
grep -q '^claudeMode=adapter$' "$CONFIG"
grep -q '^geminiMode=adapter$' "$CONFIG"

curl -fsS "$ROOM_URL/api/adapters" |
  jq -e '
    ([.adapters[] | select(.enabled == true and .state == "launchable")] | length) == 3 and
    (.adapters[] | select(.agent == "claude")).mode == "adapter" and
    (.adapters[] | select(.agent == "gemini")).mode == "adapter"
  ' >/dev/null

echo "wt-adapter-capabilities integration test passed"
