#!/usr/bin/env bash
# Phase 2 Sprint 5: policy evaluator and audit export integration test.
#
# Exercises:
#   - blocked-vendor denial + woventeam.policy_decision.v0.1 ledger record
#   - per-initiative token budget hard stop
#   - per-model-family token budget hard stop
#   - model_agent_mismatch denial (pinned agent + mismatched model)
#   - audit export endpoint and `wt-task audit` CLI streaming
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((29000 + RANDOM % 1000))}"
CONFIG="$TMPDIR/woventeam.conf"
ROOM_LOG="$TMPDIR/data/phase0-room.jsonl"
TASK_LEDGER="$TMPDIR/data/task-packages.jsonl"
PROJECTION="$TMPDIR/data/task-projection.sqlite"
RUNTIME="$TMPDIR/runtime/tasks"
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

mkdir -p "$TMPDIR/data" "$RUNTIME"
# Small per-initiative + per-family budgets so we can exercise them without
# generating thousands of fake tasks.
cat >"$CONFIG" <<CFG
roomName=policy_audit
roomLogPath=$ROOM_LOG
taskLedgerPath=$TASK_LEDGER
taskProjectionDbPath=$PROJECTION
httpBindAddress=127.0.0.1
httpPort=$PORT
fsyncEachMessage=0
roleRoutingEnabled=1
maxActiveTasksPerAgent=20
maxSubtasksPerParent=20
maxTasksPerInitiative=20
runtimeRootPath=$RUNTIME
blockedVendors=deepseek,xai
tokenBudgetPerInitiative=10000
tokenBudgetPerModelFamily=8000
tokenDailyBudget=10000000
tokenMonthlyBudget=100000000
CFG

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

initiative="init_policy_audit"

# Helper: post a task package and capture the HTTP status + body.
post_package() {
  local task_id="$1" agent="$2" model="$3" max_tokens="$4"
  curl -sS -o "$TMPDIR/last.json" -w '%{http_code}' \
    -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/task-package" <<JSON
{"schema":"woventeam.task_package.v0.1","taskId":"$task_id","initiativeId":"$initiative","createdBy":"ceo","assignedRole":"backend_dev","assignedAgent":"$agent","modelId":"$model","priority":"normal","status":"queued","title":"Policy fixture","body":"x","task":{"title":"t","body":"b"},"contextRefs":[],"acceptanceCriteria":[],"toolPolicy":{"profile":"observe"},"budget":{"maxTokens":$max_tokens},"dependencies":[],"createdAtUnixMs":1779000000000}
JSON
}

# --- 1. blocked-vendor denial ---
status="$(post_package task_blocked_vendor chatgpt deepseek/coder 100)"
test "$status" = "409"
jq -e '.ok == false and .reason == "vendor_blocked"' "$TMPDIR/last.json" >/dev/null

# --- 2. model_agent_mismatch denial (pinned agent + wrong family) ---
status="$(post_package task_mismatch claude openai/gpt-5.3-codex 100)"
test "$status" = "409"
jq -e '.ok == false and .reason == "model_agent_mismatch"' "$TMPDIR/last.json" >/dev/null

# --- 3. policy_decision ledger records ---
# Two denials so far + their routing/package events from the accepted cases
# (none yet). Verify two policy_decision records exist with the right reasons.
test "$(grep -c '"schema":"woventeam.policy_decision.v0.1"' "$TASK_LEDGER")" -eq 2
grep -q '"reason":"vendor_blocked"' "$TASK_LEDGER"
grep -q '"reason":"model_agent_mismatch"' "$TASK_LEDGER"

# --- 4. per-initiative budget hard stop ---
# Allocate 7000 tokens (under 10000 cap), then try 4000 -> should deny because
# 7000 + 4000 > 10000.
status="$(post_package task_ok_within_initiative chatgpt openai/gpt-5.3-codex 7000)"
test "$status" = "200"
status="$(post_package task_over_initiative chatgpt openai/gpt-5.3-codex 4000)"
test "$status" = "409"
jq -e '.reason == "initiative_budget"' "$TMPDIR/last.json" >/dev/null

# --- 5. per-model-family budget hard stop ---
# OpenAI family is already at 7000 (under 8000 cap). Try 2000 -> family deny.
# Note: initiative budget would also bite if we used the same initiative, so
# point this one at a different initiative so the family check fires first.
status="$(curl -sS -o "$TMPDIR/last.json" -w '%{http_code}' \
  -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/task-package" <<JSON
{"schema":"woventeam.task_package.v0.1","taskId":"task_over_family","initiativeId":"init_other","createdBy":"ceo","assignedRole":"backend_dev","assignedAgent":"chatgpt","modelId":"openai/gpt-5.3-codex","priority":"normal","status":"queued","title":"Family fixture","body":"x","task":{"title":"t","body":"b"},"contextRefs":[],"acceptanceCriteria":[],"toolPolicy":{"profile":"observe"},"budget":{"maxTokens":2000},"dependencies":[],"createdAtUnixMs":1779000000000}
JSON
)"
test "$status" = "409"
jq -e '.reason == "model_family_budget"' "$TMPDIR/last.json" >/dev/null

# --- 6. audit export endpoint contents ---
# Should include: 1 accepted task, the routing event for it, plus three
# policy denials (blocked, mismatch, initiative_budget) and pendingCount for
# acceptedArtifacts is 0.
curl -fsS "$ROOM_URL/api/initiative-audit?initiativeId=$initiative" |
  jq -e '
    .ok == true and
    .initiativeId == "'"$initiative"'" and
    .summary.taskCount == 1 and
    (.tasks | length) == 1 and
    (.tasks[0].taskId == "task_ok_within_initiative") and
    (.events | length) >= 1 and
    (.policyDecisions | length) == 3 and
    ([.policyDecisions[].reason] | sort) == ["initiative_budget","model_agent_mismatch","vendor_blocked"]
  ' >/dev/null

# --- 7. CLI parity: wt-task audit ---
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" audit "$initiative" --out "$TMPDIR/audit.json" >/dev/null
test -s "$TMPDIR/audit.json"
jq -e '.ok == true and .initiativeId == "'"$initiative"'"' "$TMPDIR/audit.json" >/dev/null

# --- 8. audit on unknown initiative returns ok:false ---
status="$(curl -sS -o "$TMPDIR/last.json" -w '%{http_code}' "$ROOM_URL/api/initiative-audit?initiativeId=init_does_not_exist")"
test "$status" = "404"
jq -e '.ok == false' "$TMPDIR/last.json" >/dev/null

echo "wt-policy-audit integration test passed"
