#!/usr/bin/env bash
# Phase 3 Sprint 5: real-time dashboard integration test.
#
# Covers the server-side surfaces the dashboard depends on:
#   - POST /api/task-priority projects the new priority on the task row
#   - POST /api/task-reroute projects assignedAgent + queued status
#   - POST /api/task-note appends a note task_event to the ledger
#   - GET /events streams a typed `event: ledger` frame within ~2s of a
#     ledger append (the mechanism behind event-driven refresh)
#
# Operator-auth on these endpoints is satisfied here because the test calls
# them over loopback (peerIsLocal bypass) - the same path the browser console
# uses. The bearer_required path for non-loopback peers is covered by
# wt-remote-execution.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((28000 + RANDOM % 1000))}"
CONFIG="$TMPDIR/woventeam.conf"
TASK_LEDGER="$TMPDIR/data/task-packages.jsonl"
ROOM_URL="http://127.0.0.1:$PORT"

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]]; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf "$TMPDIR"
}
trap cleanup EXIT

mkdir -p "$TMPDIR/data" "$TMPDIR/runtime/tasks"
cat >"$CONFIG" <<CFG
roomName=realtime
roomLogPath=$TMPDIR/data/phase0-room.jsonl
taskLedgerPath=$TASK_LEDGER
taskProjectionDbPath=$TMPDIR/data/task-projection.sqlite
httpBindAddress=127.0.0.1
httpPort=$PORT
fsyncEachMessage=0
roleRoutingEnabled=1
maxActiveTasksPerAgent=8
maxSubtasksPerParent=8
maxTasksPerInitiative=20
runtimeRootPath=$TMPDIR/runtime/tasks
CFG

"$ROOT/build/wt-roomd" --config "$CONFIG" >"$TMPDIR/server.log" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 50); do
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then cat "$TMPDIR/server.log" >&2; echo "daemon exited" >&2; exit 1; fi
  if curl -fsS "$ROOM_URL/api/health" >/dev/null 2>&1; then break; fi
  sleep 0.1
done
OPERATOR_SESSION="$(curl -fsS "$ROOM_URL/api/operator-session" | jq -r .operatorSession)"
operator_header=(-H "X-WovenTeam-Operator-Session: $OPERATOR_SESSION")

initiative="init_realtime"

# Create a task to operate on.
resp="$(WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" create \
  --title "Realtime steering target" --body "Steer me." \
  --role backend_dev --agent chatgpt --initiative "$initiative" --max-tokens 100)"
task="$(jq -r '.taskId' <<<"$resp")"
test "$task" != "null"

# --- Case 1: priority change projects ---
curl -fsS -H 'Content-Type: application/json' "${operator_header[@]}" \
  -d "{\"taskId\":\"$task\",\"priority\":\"urgent\",\"createdBy\":\"ceo\"}" \
  "$ROOM_URL/api/task-priority" | jq -e '.ok == true' >/dev/null
curl -fsS "$ROOM_URL/api/task-detail?taskId=$task" | jq -e '.task.priority == "urgent"' >/dev/null

# --- Case 2: reroute projects assignedAgent + queued status ---
curl -fsS -H 'Content-Type: application/json' "${operator_header[@]}" \
  -d "{\"taskId\":\"$task\",\"assignedAgent\":\"gemini\",\"createdBy\":\"ceo\"}" \
  "$ROOM_URL/api/task-reroute" | jq -e '.ok == true' >/dev/null
curl -fsS "$ROOM_URL/api/task-detail?taskId=$task" |
  jq -e '.task.assignedAgent == "gemini" and .task.status == "queued"' >/dev/null

# --- Case 3: operator note appends a note task_event ---
curl -fsS -H 'Content-Type: application/json' "${operator_header[@]}" \
  -d "{\"taskId\":\"$task\",\"note\":\"Watch the lease on this one.\",\"createdBy\":\"ceo\"}" \
  "$ROOM_URL/api/task-note" | jq -e '.ok == true' >/dev/null
grep -q '"eventType":"note"' "$TASK_LEDGER"
grep -q "Watch the lease on this one." "$TASK_LEDGER"

# --- Case 4: SSE ledger delta streams a new record within ~2s ---
# Connect, then trigger a ledger append and assert the typed frame arrives.
curl -sN --max-time 4 "$ROOM_URL/events" > "$TMPDIR/stream.txt" 2>/dev/null &
SSE_PID=$!
sleep 0.8
# A second priority change is a cheap, deterministic ledger append.
curl -fsS -H 'Content-Type: application/json' "${operator_header[@]}" \
  -d "{\"taskId\":\"$task\",\"priority\":\"high\",\"createdBy\":\"ceo\"}" \
  "$ROOM_URL/api/task-priority" >/dev/null
sleep 2
wait "$SSE_PID" 2>/dev/null || true
grep -q "^event: ledger" "$TMPDIR/stream.txt"
grep -q "\"taskId\":\"$task\"" "$TMPDIR/stream.txt"
# Confirm the streamed frame is the priority=high change we just made.
grep -A1 "^event: ledger" "$TMPDIR/stream.txt" | grep -q '"priority":"high"'

echo "wt-realtime-dashboard integration test passed"
