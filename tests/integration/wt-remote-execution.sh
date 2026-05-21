#!/usr/bin/env bash
set -euo pipefail

# This integration runs two remote agents over loopback because the common CI
# and Codex execution hosts do not permit unprivileged network namespaces
# (`unshare --net` and `ip netns add` fail with EPERM). A privileged acceptance
# run should place at least one agent in a separate netns/container and assert
# the same host-capability, lease-expiry, reclaim, revocation, and bearer-deny
# paths across a routed veth/container boundary instead of 127.0.0.1.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
trap 'kill "$ROOMD_PID" 2>/dev/null || true; rm -rf "$TMPDIR"' EXIT

PORT="${WT_TEST_PORT:-8799}"
ROOM_URL="http://127.0.0.1:$PORT"
CONFIG="$TMPDIR/woventeam.conf"
LEDGER="$TMPDIR/task-ledger.jsonl"
ROOM_LOG="$TMPDIR/room.jsonl"
PROJECTION="$TMPDIR/projection.sqlite"
RUNTIME="$TMPDIR/runtime"

cat >"$CONFIG" <<CONF
roomName=remote_execution
roomLogPath=$ROOM_LOG
taskLedgerPath=$LEDGER
taskProjectionDbPath=$PROJECTION
runtimeRootPath=$RUNTIME
httpBindAddress=127.0.0.1
httpPort=$PORT
leaseDurationSeconds=1
leaseRenewalIntervalSeconds=1
agentPollMilliseconds=100
enableClaudeAdapter=0
enableCodexAdapter=0
enableGeminiAdapter=0
remoteAllowedIps=127.0.0.1
authTokenDefaultTtlSeconds=60
CONF

: >"$LEDGER"
: >"$ROOM_LOG"
"$ROOT/build/wt-roomd" --config "$CONFIG" >"$TMPDIR/wt-roomd.log" 2>&1 &
ROOMD_PID=$!

for _ in $(seq 1 50); do
  if curl -fsS "$ROOM_URL/api/health" >/dev/null 2>&1; then break; fi
  if ! kill -0 "$ROOMD_PID" 2>/dev/null; then
    cat "$TMPDIR/wt-roomd.log" >&2
    exit 1
  fi
  sleep 0.1
done
curl -fsS "$ROOM_URL/api/health" >/dev/null
OPERATOR_SESSION="$(curl -fsS "$ROOM_URL/api/operator-session" | jq -r .operatorSession)"
operator_header=(-H "X-WovenTeam-Operator-Session: $OPERATOR_SESSION")

issue_token() {
  local role="$1"
  local subject="$2"
  curl -fsS -H 'Content-Type: application/json' "${operator_header[@]}" \
    -d "{\"role\":\"$role\",\"subject\":\"$subject\",\"ttlSeconds\":60}" \
    "$ROOM_URL/api/auth-token"
}

claude_token_json="$(issue_token agent claude)"
chatgpt_token_json="$(issue_token agent chatgpt)"
claude_token="$(printf '%s' "$claude_token_json" | jq -r .token)"
claude_token_id="$(printf '%s' "$claude_token_json" | jq -r .tokenId)"
chatgpt_token="$(printf '%s' "$chatgpt_token_json" | jq -r .token)"

post_capability() {
  local token="$1"
  local host="$2"
  local agent="$3"
  local profiles="$4"
  curl -fsS -H 'Content-Type: application/json' \
    -H "Authorization: Bearer $token" \
    -d "{\"host\":\"$host\",\"agent\":\"$agent\",\"profiles\":\"$profiles\",\"adapters\":\"$agent\"}" \
    "$ROOM_URL/api/host-capabilities" >/dev/null
}

post_capability "$claude_token" remote-a claude observe,ops_read
post_capability "$chatgpt_token" remote-b chatgpt observe,repo_branch,test_local

curl -fsS "$ROOM_URL/api/hosts" |
  jq -e '.ok == true and ([.hosts[] | select(.host == "remote-a" and .agent == "claude")] | length) >= 1 and ([.hosts[] | select(.host == "remote-b" and .agent == "chatgpt")] | length) >= 1' >/dev/null

submit_task() {
  local task_id="$1"
  local agent="$2"
  local model="$3"
  local host="$4"
  local profile="$5"
  curl -fsS -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/task-package" >/dev/null <<JSON
{"schema":"woventeam.task_package.v0.1","taskId":"$task_id","initiativeId":"init_remote_execution","createdBy":"ceo","assignedRole":"backend_dev","assignedAgent":"$agent","modelId":"$model","priority":"normal","executionHost":"$host","status":"queued","title":"Remote task $task_id","body":"Run on $host.","task":{"title":"Remote task","body":"Run remotely.","deliverables":[]},"contextRefs":[],"acceptanceCriteria":["Remote task completes."],"toolPolicy":{"profile":"$profile","filesystem":"read_only","network":"none","system":"none","git":"none"},"budget":{"timeoutSeconds":10,"maxOutputBytes":1048576,"maxCostUsd":0.01},"dependencies":[],"createdAtUnixMs":1778915000000}
JSON
}

submit_task task_remote_a claude anthropic/claude-sonnet remote-a observe
submit_task task_remote_b chatgpt openai/gpt-5.3-codex remote-b test_local

"$ROOT/build/wt-agent" --agent claude --once --remote "$ROOM_URL" --bearer "$claude_token" --host remote-a
"$ROOT/build/wt-agent" --agent chatgpt --once --remote "$ROOM_URL" --bearer "$chatgpt_token" --host remote-b

curl -fsS "$ROOM_URL/api/task-detail?taskId=task_remote_a" |
  jq -e '.task.status == "complete" and ([.events[] | select(.eventType == "lease")] | length) == 1' >/dev/null
curl -fsS "$ROOM_URL/api/task-detail?taskId=task_remote_b" |
  jq -e '.task.status == "complete" and ([.events[] | select(.eventType == "lease")] | length) == 1' >/dev/null

submit_task task_partition claude anthropic/claude-sonnet remote-a observe
curl -fsS -H 'Content-Type: application/json' \
  -H "Authorization: Bearer $claude_token" \
  -d '{"host":"remote-a","agent":"claude"}' "$ROOM_URL/api/remote-claim" |
  jq -e '.ok == true and .claimed == true and .taskId == "task_partition"' >/dev/null
sleep 2
"$ROOT/build/wt-agent" --agent claude --once --remote "$ROOM_URL" --bearer "$claude_token" --host remote-a
curl -fsS "$ROOM_URL/api/task-detail?taskId=task_partition" |
  jq -e '.task.status == "complete" and .task.lastReclaimReason == "lease_expired" and ([.events[] | select(.eventType == "reclaim")] | length) == 1' >/dev/null

curl -fsS -H 'Content-Type: application/json' "${operator_header[@]}" \
  -d "{\"tokenId\":\"$claude_token_id\"}" "$ROOM_URL/api/auth-token/revoke" |
  jq -e '.ok == true and .status == "revoked"' >/dev/null
if "$ROOT/build/wt-agent" --agent claude --once --remote "$ROOM_URL" --bearer "$claude_token" --host remote-a 2>"$TMPDIR/revoked.err"; then
  echo "revoked token unexpectedly claimed work" >&2
  exit 1
fi
grep -q "token_revoked" "$TMPDIR/revoked.err"

status="$(curl -sS -o "$TMPDIR/capability.json" -w '%{http_code}' \
  -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/task-package" <<JSON
{"schema":"woventeam.task_package.v0.1","taskId":"task_capability_unmet","initiativeId":"init_remote_execution","createdBy":"ceo","assignedRole":"backend_dev","assignedAgent":"claude","modelId":"anthropic/claude-sonnet","priority":"normal","executionHost":"remote-a","status":"queued","title":"Capability unmet","body":"Bad profile.","task":{"title":"Bad profile","body":"Bad profile.","deliverables":[]},"contextRefs":[],"acceptanceCriteria":["Reject."],"toolPolicy":{"profile":"repo_branch","filesystem":"workspace_write","network":"none","system":"none","git":"branch_only"},"budget":{"timeoutSeconds":10,"maxOutputBytes":1048576,"maxCostUsd":0.01},"dependencies":[],"createdAtUnixMs":1778915000000}
JSON
)"
test "$status" = "409"
jq -e '.reason == "capability_unmet"' "$TMPDIR/capability.json" >/dev/null

status="$(curl -sS -o "$TMPDIR/anon.json" -w '%{http_code}' \
  -H 'Content-Type: application/json' \
  -d '{"agent":"claude","taskId":"task_remote_a","status":"running"}' \
  "$ROOM_URL/api/remote-task-event")"
test "$status" = "401"
jq -e '.reason == "bearer_required"' "$TMPDIR/anon.json" >/dev/null
if grep -q "$claude_token" "$LEDGER"; then
  echo "raw bearer token leaked to ledger" >&2
  exit 1
fi
grep -q '"tokenHash":"sha256:' "$LEDGER"

echo "wt-remote-execution integration test passed"
