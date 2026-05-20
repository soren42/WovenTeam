#!/usr/bin/env bash
# Phase 2 launch-era end-to-end harness.
#
# Folds Sprint 3-5 deliverables into one repeatable rehearsal:
#   - initiative creation + routing
#   - blocked-vendor policy denial recorded as policy_decision event
#   - per-initiative budget denial
#   - real-adapter preflight surfaces classified state
#   - lease + auto-reclaim recovery
#   - artifact promotion (draft -> reviewed -> accepted)
#   - operator-triggered reclaim from CLI
#   - audit export contains accepted + denied + reclaimed history
#   - wt-roomd restart rebuilds the projection from JSONL without loss
#
# The harness uses the existing fake-claude trick (deterministic stub) so it
# never invokes an external CLI. The opt-in live rehearsal lives in
# bin/wt-rehearse-live and is operator-gated.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((30000 + RANDOM % 1000))}"
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
# Deterministic fake Claude binary so the adapter run does not require any
# external CLI to be installed.
cat >"$TMPDIR/fake-claude" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf 'Phase 2 e2e fake Claude result\n'
SH
chmod +x "$TMPDIR/fake-claude"

cat >"$CONFIG" <<CFG
roomName=phase2_e2e
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
tokenTelemetryEnabled=1
tokenDailyBudget=10000000
tokenMonthlyBudget=100000000
tokenBudgetPerInitiative=4000
tokenBudgetPerModelFamily=0
blockedVendors=deepseek,xai
enableClaudeAdapter=1
claudeMode=adapter
claudeCommand=$TMPDIR/fake-claude
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
    if curl -fsS "$ROOM_URL/api/health" >/dev/null 2>&1; then return; fi
    sleep 0.1
  done
  curl -fsS "$ROOM_URL/api/health" >/dev/null
}

restart_roomd() {
  # Stop the daemon, drop the SQLite projection so the rebuild path is
  # exercised, then restart and confirm health.
  kill "$SERVER_PID" 2>/dev/null || true
  wait "$SERVER_PID" 2>/dev/null || true
  SERVER_PID=""
  rm -f "$PROJECTION" "$PROJECTION-shm" "$PROJECTION-wal"
  start_roomd
}

start_roomd

# 1. Health + adapter readiness ---------------------------------------------
curl -fsS "$ROOM_URL/api/health" |
  jq -e '.ok == true and .projection.ok == true and .adapters.claudeEnabled == true' >/dev/null
curl -fsS "$ROOM_URL/api/adapters" |
  jq -e '.adapters[] | select(.agent == "claude" and .enabled == true and .state == "launchable")' >/dev/null

initiative="init_phase2_e2e"

# 2. Initiative + routed parent task ----------------------------------------
parent_response="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" create \
    --title "Phase 2 launch rehearsal manager" \
    --body "Drive an adapter-backed deliverable through full Phase 2 controls." \
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

# 3. Blocked-vendor denial: deepseek hits the policy evaluator first --------
status="$(curl -sS -o "$TMPDIR/blocked.json" -w '%{http_code}' \
  -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/task-package" <<JSON
{"schema":"woventeam.task_package.v0.1","taskId":"task_phase2_blocked","initiativeId":"$initiative","createdBy":"phase2-e2e","assignedRole":"backend_dev","assignedAgent":"chatgpt","modelId":"deepseek/coder","priority":"normal","status":"queued","title":"Should be denied","body":"x","task":{"title":"t","body":"b"},"contextRefs":[],"acceptanceCriteria":[],"toolPolicy":{"profile":"observe"},"budget":{"maxTokens":1000},"dependencies":[],"createdAtUnixMs":1779000000000}
JSON
)"
test "$status" = "409"
jq -e '.reason == "vendor_blocked"' "$TMPDIR/blocked.json" >/dev/null

# 4. Routed adapter child + lease + completion ------------------------------
child_response="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" request \
    --parent "$parent_id" \
    --by-role project_manager \
    --by claude \
    --role technical_writer \
    --title "Phase 2 routed adapter task" \
    --body "Produce the launch rehearsal proof artifact." \
    --agent router \
    --initiative "$initiative" \
    --tool-profile observe \
    --max-tokens 2000
)"
child_id="$(jq -r '.taskId' <<<"$child_response")"
test "$child_id" != "null"
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" update-status "$parent_id" --status complete --message "rehearsal manager accepted child" >/dev/null

# Run the agent against the fake-claude binary; this exercises lease + adapter
# + status events end to end.
WT_ROOM_LOG_PATH="$ROOM_LOG" \
WT_TASK_LEDGER_PATH="$TASK_LEDGER" \
WT_TASK_PROJECTION_DB_PATH="$PROJECTION" \
WT_RUNTIME_ROOT_PATH="$RUNTIME" \
WT_ENABLE_CLAUDE_ADAPTER=1 \
WT_CLAUDE_MODE=adapter \
WT_CLAUDE_COMMAND="$TMPDIR/fake-claude" \
  "$ROOT/build/wt-agent" --agent claude --once >/dev/null

curl -fsS "$ROOM_URL/api/task-detail?taskId=$child_id" |
  jq -e '
    .task.status == "complete" and
    .task.attemptCount == 1 and
    ([.events[] | select(.eventType == "lease" and .status == "leased")] | length) == 1
  ' >/dev/null

# 5. Per-initiative budget denial -------------------------------------------
# Initiative cap = 4000. Whether or not the parent/child are still active, a
# single 5000-token request must always exceed the cap. Deliberately pinning
# agent + model so the request bypasses routing's auto-default.
status="$(curl -sS -o "$TMPDIR/budget.json" -w '%{http_code}' \
  -H 'Content-Type: application/json' -d @- "$ROOM_URL/api/task-package" <<JSON
{"schema":"woventeam.task_package.v0.1","taskId":"task_phase2_overbudget","initiativeId":"$initiative","createdBy":"phase2-e2e","assignedRole":"backend_dev","assignedAgent":"chatgpt","modelId":"openai/gpt-5.3-codex","priority":"normal","status":"queued","title":"Over budget","body":"x","task":{"title":"t","body":"b"},"contextRefs":[],"acceptanceCriteria":[],"toolPolicy":{"profile":"observe"},"budget":{"maxTokens":5000},"dependencies":[],"createdAtUnixMs":1779000000000}
JSON
)"
test "$status" = "409"
jq -e '.reason == "initiative_budget"' "$TMPDIR/budget.json" >/dev/null

# 6. Artifact promotion -----------------------------------------------------
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" artifact review "$child_id" \
  --path result.md --reviewer rehearsal --notes "spot checks passed" >/dev/null
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" artifact promote "$child_id" \
  --path result.md --reviewer ceo --notes "ships" >/dev/null
curl -fsS "$ROOM_URL/api/task-detail?taskId=$child_id" |
  jq -e '
    .task.artifactState == "accepted" and
    .task.acceptedArtifactPath == "result.md" and
    .task.lastReviewer == "ceo"
  ' >/dev/null

# 7. Operator-triggered reclaim ---------------------------------------------
# Drop a queued task and reclaim it; verify the reclaim event lands and the
# projection shows reclaimCount == 1.
reclaim_response="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" create \
    --title "Phase 2 reclaim coverage" \
    --body "Validate operator reclaim from CLI." \
    --role backend_dev \
    --agent chatgpt \
    --initiative "$initiative" \
    --max-tokens 200
)"
reclaim_task="$(jq -r '.taskId' <<<"$reclaim_response")"
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" reclaim "$reclaim_task" \
  --reason operator --message "rehearsal operator reclaim" >/dev/null
curl -fsS "$ROOM_URL/api/task-detail?taskId=$reclaim_task" |
  jq -e '.task.reclaimCount == 1 and .task.lastReclaimReason == "operator"' >/dev/null

# 8. Audit export combines accepted + denied + reclaimed --------------------
curl -fsS "$ROOM_URL/api/initiative-audit?initiativeId=$initiative" >"$TMPDIR/audit.json"
jq -e --arg parent "$parent_id" --arg child "$child_id" --arg reclaim "$reclaim_task" '
    .ok == true and
    .initiativeId == "'"$initiative"'" and
    (.tasks | map(.taskId) | sort) == ([$child, $parent, $reclaim] | sort) and
    (.summary.acceptedArtifacts == 1) and
    ([.policyDecisions[].reason] | sort) == ["initiative_budget","vendor_blocked"]
  ' "$TMPDIR/audit.json" >/dev/null

# 9. wt-task audit CLI parity -----------------------------------------------
WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" audit "$initiative" --out "$TMPDIR/audit-cli.json" >/dev/null
test -s "$TMPDIR/audit-cli.json"
jq -e '.ok == true' "$TMPDIR/audit-cli.json" >/dev/null

# 10. Restart and recovery --------------------------------------------------
restart_roomd
curl -fsS "$ROOM_URL/api/health" |
  jq -e '.ok == true and .projection.ok == true' >/dev/null
# After restart the projection rebuilds from JSONL; the audit endpoint should
# return the same task set and accepted artifact count.
curl -fsS "$ROOM_URL/api/initiative-audit?initiativeId=$initiative" |
  jq -e --arg parent "$parent_id" --arg child "$child_id" --arg reclaim "$reclaim_task" '
    .ok == true and
    .summary.acceptedArtifacts == 1 and
    (.tasks | map(.taskId) | sort) == ([$child, $parent, $reclaim] | sort)
  ' >/dev/null

echo "wt-phase2-e2e integration test passed"
