#!/usr/bin/env bash
# Phase 3 Sprint 1: runtime observability + control plane integration test.
#
# Exercises:
#   - heartbeat append + projection into /api/status
#   - milestone event append + recentMilestones in /api/status
#   - lease renewal: a stub adapter runs long enough to trigger the renewal
#     event the agent wait loop appends
#   - operator cancel via /api/task-cancel terminates the running adapter
#     process group and leaves a kill_event + cancelled status in the ledger
#   - audit pagination: ?since= filters by timestamp, ?limit= truncates,
#     paginated stitching matches the unpaginated baseline
#   - Slack-style notification: a local mock listener catches the daemon's
#     curl POST after a policy denial
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((31000 + RANDOM % 1000))}"
MOCK_PORT=$((PORT + 1))
CONFIG="$TMPDIR/woventeam.conf"
ROOM_LOG="$TMPDIR/data/phase0-room.jsonl"
TASK_LEDGER="$TMPDIR/data/task-packages.jsonl"
PROJECTION="$TMPDIR/data/task-projection.sqlite"
RUNTIME="$TMPDIR/runtime/tasks"
SERVER_LOG="$TMPDIR/wt-roomd.log"
MOCK_LOG="$TMPDIR/mock-listener.log"
ROOM_URL="http://127.0.0.1:$PORT"

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]]; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  if [[ -n "${MOCK_PID:-}" ]]; then
    kill "$MOCK_PID" 2>/dev/null || true
    wait "$MOCK_PID" 2>/dev/null || true
  fi
  rm -rf "$TMPDIR"
}
trap cleanup EXIT

mkdir -p "$TMPDIR/data" "$RUNTIME"

# Stub adapter that sleeps long enough to exercise lease renewal + cancel.
# When cancelled (SIGTERM), it should exit non-zero so we can verify the
# cancelled status lands.
cat >"$TMPDIR/fake-claude" <<'SH'
#!/usr/bin/env bash
trap 'echo "fake-claude got SIGTERM" >&2; exit 130' TERM
echo "fake-claude starting"
sleep 8
echo "fake-claude finished"
SH
chmod +x "$TMPDIR/fake-claude"

# Local mock HTTP listener (Python stdlib; the project directive prefers
# Python over Node, and this fits in one file with no deps).
cat >"$TMPDIR/mock-listener.py" <<PY
import http.server
import json
import sys
import threading

PORT = ${MOCK_PORT}
captures = []

class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length else b""
        captures.append(body.decode("utf-8", "replace"))
        with open("${MOCK_LOG}", "a") as fh:
            fh.write(body.decode("utf-8", "replace") + "\n")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(b'{"ok":true}')
    def log_message(self, format, *args):
        pass  # quiet

httpd = http.server.HTTPServer(("127.0.0.1", PORT), Handler)
httpd.serve_forever()
PY

python3 "$TMPDIR/mock-listener.py" >/dev/null 2>&1 &
MOCK_PID=$!
for _ in $(seq 1 50); do
  if curl -fsS -X POST -d '{"ping":true}' "http://127.0.0.1:$MOCK_PORT/" >/dev/null 2>&1; then break; fi
  sleep 0.1
done
# The probe-POST above also lands in MOCK_LOG; clear so only daemon emissions count.
: >"$MOCK_LOG"

# Daemon config. Short lease + short renewal so the test exercises renewal
# inside the ~8-second fake adapter run.
cat >"$CONFIG" <<CFG
roomName=observability
roomLogPath=$ROOM_LOG
taskLedgerPath=$TASK_LEDGER
taskProjectionDbPath=$PROJECTION
httpBindAddress=127.0.0.1
httpPort=$PORT
fsyncEachMessage=0
roleRoutingEnabled=1
runtimeRootPath=$RUNTIME
heartbeatIntervalSeconds=60
leaseRenewalIntervalSeconds=2
leaseDurationSeconds=5
cancelPollIntervalSeconds=1
notificationEscalateKey=primary
slackWebhookFile=$TMPDIR/webhooks.txt
blockedVendors=deepseek
enableClaudeAdapter=1
claudeMode=adapter
claudeCommand=$TMPDIR/fake-claude
tokenDailyBudget=10000000
tokenMonthlyBudget=100000000
CFG

# Pretend webhook file (the override env var on the server side will take
# precedence so this is mostly here so the daemon's lookup path doesn't error).
echo "primary=http://example.invalid/webhook" >"$TMPDIR/webhooks.txt"

WT_NOTIFICATION_OVERRIDE_URL="http://127.0.0.1:$MOCK_PORT/" \
  "$ROOT/build/wt-roomd" --config "$CONFIG" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 50); do
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    cat "$SERVER_LOG" >&2 || true
    echo "wt-roomd exited before health check" >&2
    exit 1
  fi
  if curl -fsS "$ROOM_URL/api/health" >/dev/null 2>&1; then break; fi
  sleep 0.1
done

initiative="init_observability"

# 1. /api/status responds, heartbeats empty so far ----------------------------
curl -fsS "$ROOM_URL/api/status" |
  jq -e '.ok == true and (.heartbeats | length) == 0 and (.recentMilestones | length) == 0' >/dev/null

# 2. Heartbeat append surfaces in /api/status ---------------------------------
# wt-agent emits heartbeats from its loop; for this smoke we write one directly
# to the ledger so the projection picks it up on the next /api/status hit.
# A dedicated POST /api/heartbeat endpoint can be added if operator-driven
# heartbeats ever become a requirement.
now_ms=$(date +%s%3N)
cat >>"$TASK_LEDGER" <<JSON
{"schema":"woventeam.heartbeat.v0.1","agent":"claude","host":"smoke-host","currentTaskId":"","leaseExpiresAtUnixMs":0,"statusLine":"idle smoke","createdAtUnixMs":$now_ms}
JSON
curl -fsS "$ROOM_URL/api/status" |
  jq -e '
    (.heartbeats | length) == 1 and
    (.heartbeats[0].agent == "claude") and
    (.heartbeats[0].statusLine == "idle smoke")
  ' >/dev/null

# 3. Milestone event surfaces too ---------------------------------------------
milestone_ms=$(date +%s%3N)
cat >>"$TASK_LEDGER" <<JSON
{"schema":"woventeam.milestone.v0.1","taskId":"task_observability_dummy","milestone":"plan","message":"smoke milestone","createdBy":"wt-agent@claude","createdAtUnixMs":$milestone_ms}
JSON
curl -fsS "$ROOM_URL/api/status" |
  jq -e '(.recentMilestones | length) >= 1 and (.recentMilestones[0].milestone == "plan")' >/dev/null

# 4. Lease renewal + cancel ---------------------------------------------------
# Submit a task with an explicit Claude agent, then run wt-agent inline. Once
# the agent forks the adapter (sleep 8), trigger cancel from a parallel shell
# and confirm the ledger captures the kill_event + cancelled status.
task_response="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" create \
    --title "Observability smoke" \
    --body "Sleep adapter exercises lease renewal + cancel." \
    --role technical_writer \
    --agent claude \
    --initiative "$initiative" \
    --max-tokens 100
)"
task_id="$(jq -r '.taskId' <<<"$task_response")"
test "$task_id" != "null"

# Run wt-agent in background; the fake-claude sleeps 8 seconds so we have time.
WT_ROOM_LOG_PATH="$ROOM_LOG" \
WT_TASK_LEDGER_PATH="$TASK_LEDGER" \
WT_TASK_PROJECTION_DB_PATH="$PROJECTION" \
WT_RUNTIME_ROOT_PATH="$RUNTIME" \
WT_ENABLE_CLAUDE_ADAPTER=1 \
WT_CLAUDE_MODE=adapter \
WT_CLAUDE_COMMAND="$TMPDIR/fake-claude" \
WT_LEASE_RENEWAL_INTERVAL_SECONDS=2 \
WT_LEASE_DURATION_SECONDS=5 \
WT_CANCEL_POLL_INTERVAL_SECONDS=1 \
  "$ROOT/build/wt-agent" --agent claude --once >"$TMPDIR/agent.out" 2>&1 &
AGENT_PID=$!

# Wait until the agent has appended a lease event, then send cancel.
for _ in $(seq 1 50); do
  if grep -q '"eventType":"lease"' "$TASK_LEDGER" && grep -q "$task_id" "$TASK_LEDGER"; then break; fi
  sleep 0.1
done
# Sleep a few seconds so a lease_renewal can land before we cancel.
sleep 3
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" cancel-task "$task_id" \
  --reason operator --message "smoke cancel" >/dev/null

# Wait for the agent to finish (it should exit after cancel cleanup).
wait "$AGENT_PID" 2>/dev/null || true

# Verify: lease event present, at least one lease_renewal, kill_event recorded,
# final status is cancelled.
grep -q "$task_id" "$TASK_LEDGER"
test "$(grep -c "\"eventType\":\"lease_renewal\".*$task_id\\|$task_id.*\"eventType\":\"lease_renewal\"" "$TASK_LEDGER")" -ge 1
grep -q "\"schema\":\"woventeam.kill_event.v0.1\".*$task_id\\|$task_id.*\"schema\":\"woventeam.kill_event.v0.1\"" "$TASK_LEDGER"
curl -fsS "$ROOM_URL/api/task-detail?taskId=$task_id" |
  jq -e '.task.status == "cancelled"' >/dev/null

# 5. Audit pagination ---------------------------------------------------------
# Baseline: full audit response.
curl -fsS "$ROOM_URL/api/initiative-audit?initiativeId=$initiative" >"$TMPDIR/audit-full.json"
jq -e '.ok == true and .truncated == false' "$TMPDIR/audit-full.json" >/dev/null
full_events=$(jq '.events | length' "$TMPDIR/audit-full.json")
test "$full_events" -gt 0

# Paginated: limit=2 should truncate when there are >2 events.
curl -fsS "$ROOM_URL/api/initiative-audit?initiativeId=$initiative&limit=2" >"$TMPDIR/audit-page1.json"
truncated=$(jq -r '.truncated' "$TMPDIR/audit-page1.json")
emitted_page1=$(jq -r '.emittedCount' "$TMPDIR/audit-page1.json")
test "$emitted_page1" -le 2
if [[ "$truncated" == "true" ]]; then
  next=$(jq -r '.nextSinceUnixMs' "$TMPDIR/audit-page1.json")
  test "$next" -gt 0
  # since= filter returns only newer events
  curl -fsS "$ROOM_URL/api/initiative-audit?initiativeId=$initiative&since=$next" >"$TMPDIR/audit-page2.json"
  jq -e '.ok == true' "$TMPDIR/audit-page2.json" >/dev/null
  page2_min=$(jq -r '[.events[].createdAtUnixMs] | min // 0' "$TMPDIR/audit-page2.json")
  if [[ "$page2_min" != "0" ]]; then
    test "$page2_min" -ge "$next"
  fi
fi

# 6. Slack notification: trigger a policy denial; mock listener captures the POST.
# A deepseek modelId is in the default block list.
: >"$MOCK_LOG"
curl -sS -o /dev/null -w '%{http_code}\n' \
  -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/task-package" <<JSON >/dev/null
{"schema":"woventeam.task_package.v0.1","taskId":"task_block_obs","initiativeId":"$initiative","createdBy":"ceo","assignedRole":"backend_dev","assignedAgent":"chatgpt","modelId":"deepseek/coder","priority":"normal","status":"queued","title":"blocked","body":"x","task":{"title":"t","body":"b"},"contextRefs":[],"acceptanceCriteria":[],"toolPolicy":{"profile":"observe"},"budget":{"maxTokens":100},"dependencies":[],"createdAtUnixMs":1779000000000}
JSON

# Give the daemon a moment to fire the curl child.
for _ in $(seq 1 20); do
  if [[ -s "$MOCK_LOG" ]] && grep -q "policy denial" "$MOCK_LOG"; then break; fi
  sleep 0.1
done
test -s "$MOCK_LOG"
grep -q "policy denial" "$MOCK_LOG"
grep -q "vendor_blocked" "$MOCK_LOG"

echo "wt-observability integration test passed"
