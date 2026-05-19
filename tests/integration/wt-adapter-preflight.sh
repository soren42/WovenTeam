#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((25000 + RANDOM % 1000))}"
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

mkdir -p "$TMPDIR/runtime"
cat >"$TMPDIR/fake-codex" <<'SH'
#!/usr/bin/env bash
echo fake codex
SH
cat >"$TMPDIR/fake-claude" <<'SH'
#!/usr/bin/env bash
echo fake claude
SH
chmod +x "$TMPDIR/fake-codex" "$TMPDIR/fake-claude"

cat >"$TASK_LEDGER" <<'JSONL'
{"schema":"woventeam.task_event.v0.1","taskId":"task_failed_adapter","eventType":"status","status":"failed","message":"claude adapter exitCode=127 timedOut=false; see task workspace manifest.","createdBy":"wt-agent@claude","createdAtUnixMs":1778915000000}
JSONL

cat >"$CONFIG" <<EOF_CONFIG
roomName=phase2_preflight
roomLogPath=$ROOM_LOG
taskLedgerPath=$TASK_LEDGER
taskProjectionDbPath=$TASK_DB
httpBindAddress=127.0.0.1
httpPort=$PORT
contextMessageCount=20
agentPollMilliseconds=1000
adapterTimeoutSeconds=10
adapterMaxOutputBytes=1048576
fsyncEachMessage=0
enableCodexAdapter=1
enableClaudeAdapter=1
enableGeminiAdapter=1
tokenTelemetryEnabled=1
tokenDailyBudget=2000000
tokenMonthlyBudget=50000000
tokenWarningPercent=80
tokenCostPerMillionCents=1000
runtimeRootPath=$TMPDIR/runtime
claudeMode=adapter
chatgptMode=stub
geminiMode=stub
claudeCommand=$TMPDIR/fake-claude
gptCommand=$TMPDIR/fake-codex
geminiCommand=$TMPDIR/missing-gemini
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
    (.adapters[] | select(.agent == "chatgpt")).preflight.ok == true and
    (.adapters[] | select(.agent == "claude")).preflight.ok == true and
    (.adapters[] | select(.agent == "claude")).preflight.lastFailure.class == "missing_cli" and
    (.adapters[] | select(.agent == "gemini")).preflight.ok == false and
    (.adapters[] | select(.agent == "gemini")).preflight.reason == "adapter mode must be adapter"
  ' >/dev/null

jq -cn --arg gemini "$TMPDIR/missing-gemini" '{geminiMode:"adapter",geminiCommand:$gemini}' |
  curl -fsS -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/config" >/dev/null

curl -fsS "$ROOM_URL/api/adapters" |
  jq -e '
    (.adapters[] | select(.agent == "gemini")).preflight.ok == false and
    (.adapters[] | select(.agent == "gemini")).preflight.reason == "command is missing or not executable" and
    (.adapters[] | select(.agent == "gemini")).preflight.checks.commandExecutable == false
  ' >/dev/null

WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-adapter-preflight" --json |
  jq -e '.ok == true and ([.adapters[] | select(.preflight.ok == false)] | length) == 1' >/dev/null
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-adapter-preflight" |
  grep -q 'gemini'

echo "wt-adapter-preflight integration test passed"
