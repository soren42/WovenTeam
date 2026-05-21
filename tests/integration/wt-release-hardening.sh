#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((29000 + RANDOM % 1000))}"
ROOM_URL="http://127.0.0.1:$PORT"
CONFIG="$TMPDIR/woventeam.conf"
LEDGER="$TMPDIR/data/task-packages.jsonl"
ROOM_LOG="$TMPDIR/data/room.jsonl"
PROJECTION="$TMPDIR/data/task-projection.sqlite"
RUNTIME="$TMPDIR/runtime/tasks"
DELIVERABLES="$TMPDIR/deliverables"

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]]; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf "$TMPDIR"
}
trap cleanup EXIT

mkdir -p "$TMPDIR/data" "$RUNTIME" "$DELIVERABLES"
mkdir -p "$TMPDIR/bin"
cat >"$TMPDIR/bin/claude" <<'SH'
#!/usr/bin/env bash
printf 'adapter result with wt_1234567890abcdef0123456789abcdef secret\n'
SH
chmod +x "$TMPDIR/bin/claude"
cat >"$CONFIG" <<CFG
roomName=release_hardening
roomLogPath=$ROOM_LOG
taskLedgerPath=$LEDGER
taskProjectionDbPath=$PROJECTION
httpBindAddress=127.0.0.1
httpPort=$PORT
runtimeRootPath=$RUNTIME
deliverableRoot=$DELIVERABLES
enableClaudeAdapter=1
claudeMode=adapter
claudeCommand=$TMPDIR/bin/claude
authTokenDefaultTtlSeconds=1
remoteAllowedIps=127.0.0.1
CFG
: >"$LEDGER"
: >"$ROOM_LOG"

"$ROOT/build/wt-roomd" --config "$CONFIG" >"$TMPDIR/server.log" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 50); do
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then cat "$TMPDIR/server.log" >&2; exit 1; fi
  if curl -fsS "$ROOM_URL/api/health" >/dev/null 2>&1; then break; fi
  sleep 0.1
done
curl -fsS "$ROOM_URL/api/health" >/dev/null

OPERATOR_SESSION="$(curl -fsS "$ROOM_URL/api/operator-session" | jq -r .operatorSession)"
operator_header=(-H "X-WovenTeam-Operator-Session: $OPERATOR_SESSION")

initiative="init_release_hardening"
resp="$(WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" create \
  --title "Release hardening replay target" \
  --body "Exercise restore and audit surfaces." \
  --role backend_dev --agent chatgpt --initiative "$initiative" --max-tokens 100)"
task_id="$(jq -r .taskId <<<"$resp")"
mkdir -p "$RUNTIME/$task_id"
printf 'release hardening result\n' >"$RUNTIME/$task_id/result.md"
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" artifact promote "$task_id" \
  --path result.md --reviewer ceo --notes "accepted for replay rehearsal" >/dev/null
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" artifact ship "$task_id" \
  --mode copy --reviewer ceo --dest-basename result.md >/dev/null

curl -fsS "$ROOM_URL/api/task-detail?taskId=$task_id" | jq -S . >"$TMPDIR/status.baseline.json"
curl -fsS "$ROOM_URL/api/initiative-audit?initiativeId=$initiative" | jq -S . >"$TMPDIR/audit.baseline.json"
curl -fsS "$ROOM_URL/api/initiative-artifacts?initiativeId=$initiative" | jq -S . >"$TMPDIR/deliverables.baseline.json"

rm -f "$PROJECTION"
curl -fsS "$ROOM_URL/api/task-detail?taskId=$task_id" | jq -S . >"$TMPDIR/status.replayed.json"
curl -fsS "$ROOM_URL/api/initiative-audit?initiativeId=$initiative" | jq -S . >"$TMPDIR/audit.replayed.json"
curl -fsS "$ROOM_URL/api/initiative-artifacts?initiativeId=$initiative" | jq -S . >"$TMPDIR/deliverables.replayed.json"
cmp "$TMPDIR/status.baseline.json" "$TMPDIR/status.replayed.json"
cmp "$TMPDIR/audit.baseline.json" "$TMPDIR/audit.replayed.json"
cmp "$TMPDIR/deliverables.baseline.json" "$TMPDIR/deliverables.replayed.json"

status="$(curl -sS -o "$TMPDIR/unauth.json" -w '%{http_code}' \
  -H 'Content-Type: application/json' \
  -d "{\"taskId\":\"$task_id\",\"priority\":\"urgent\"}" \
  "$ROOM_URL/api/task-priority")"
test "$status" = "401"
jq -e '.reason == "bearer_required"' "$TMPDIR/unauth.json" >/dev/null
curl -fsS -H 'Content-Type: application/json' "${operator_header[@]}" \
  -d "{\"taskId\":\"$task_id\",\"priority\":\"high\"}" \
  "$ROOM_URL/api/task-priority" | jq -e '.ok == true' >/dev/null

expired="$(curl -fsS -H 'Content-Type: application/json' "${operator_header[@]}" \
  -d '{"role":"agent","subject":"claude","ttlSeconds":1}' "$ROOM_URL/api/auth-token")"
expired_token="$(jq -r .token <<<"$expired")"
sleep 2
status="$(curl -sS -o "$TMPDIR/expired.json" -w '%{http_code}' \
  -H 'Content-Type: application/json' -H "Authorization: Bearer $expired_token" \
  -d '{"host":"remote-hardening","agent":"claude"}' "$ROOM_URL/api/remote-claim")"
test "$status" = "401"
jq -e '.reason == "token_expired"' "$TMPDIR/expired.json" >/dev/null
! grep -q "$expired_token" "$LEDGER"

printf '{"schema":"woventeam.task_event.v0.1","taskId":"%s","eventType":"note","status":"queued","message":"oversized audit response %02048d","createdBy":"tester","createdAtUnixMs":1778915000000}\n' "$task_id" 1 >>"$LEDGER"
printf '{corrupted jsonl line\n' >>"$LEDGER"
curl -fsS "$ROOM_URL/api/initiative-audit?initiativeId=$initiative" |
  jq -e '.ok == true and (.events | length) >= 3' >/dev/null

adapter_resp="$(WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" create \
  --title "Secret redaction source test" \
  --body "Adapter emits a token-shaped string." \
  --role technical_writer --agent claude --initiative "$initiative" --max-tokens 100)"
adapter_task="$(jq -r .taskId <<<"$adapter_resp")"
"$ROOT/build/wt-agent" --agent claude --once --config "$CONFIG" >/dev/null
! grep -R "wt_1234567890abcdef0123456789abcdef" "$RUNTIME/$adapter_task"
grep -R "\\[REDACTED\\]" "$RUNTIME/$adapter_task" >/dev/null

echo "wt-release-hardening integration test passed"
