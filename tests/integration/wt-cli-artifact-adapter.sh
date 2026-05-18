#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

ROOM_LOG="$TMPDIR/phase0-room.jsonl"
TASK_LEDGER="$TMPDIR/task-packages.jsonl"
RUNTIME_ROOT="$TMPDIR/runtime/tasks"
TASK_ID="task_claude_adapter_001"

cat > "$TMPDIR/fake-claude" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
prompt="$*"
printf 'fake claude artifact\n'
printf '%s\n' "$prompt" | grep -q -- '--print'
printf '%s\n' "$prompt" | grep -q 'Task ID: task_claude_adapter_001'
printf 'fake claude stderr\n' >&2
SH
chmod +x "$TMPDIR/fake-claude"

cat > "$TASK_LEDGER" <<JSON
{"schema":"woventeam.task_package.v0.1","taskId":"$TASK_ID","initiativeId":"init_claude_adapter","createdBy":"ceo","assignedRole":"technical_writer","assignedAgent":"claude","modelId":"anthropic/claude-sonnet","priority":"normal","status":"queued","title":"Claude adapter smoke","body":"Produce a fake planning artifact.","task":{"title":"Claude adapter smoke","body":"Produce a fake planning artifact.","deliverables":[]},"contextRefs":[],"acceptanceCriteria":["Fake Claude result is captured."],"toolPolicy":{"profile":"observe","filesystem":"read_only","network":"none","system":"none","git":"none"},"budget":{"timeoutSeconds":10,"maxOutputBytes":1048576,"maxCostUsd":0.01},"dependencies":[],"createdAtUnixMs":1778915000000}
JSON

WT_ROOM_LOG_PATH="$ROOM_LOG" \
WT_TASK_LEDGER_PATH="$TASK_LEDGER" \
WT_ENABLE_CLAUDE_ADAPTER=1 \
WT_CLAUDE_MODE=adapter \
WT_RUNTIME_ROOT_PATH="$RUNTIME_ROOT" \
WT_CLAUDE_COMMAND="$TMPDIR/fake-claude" \
  "$ROOT/build/wt-agent" --agent claude --once > "$TMPDIR/agent.out"

WORKSPACE="$RUNTIME_ROOT/$TASK_ID"
test -f "$WORKSPACE/prompt.md"
test -f "$WORKSPACE/result.md"
test -f "$WORKSPACE/stdout.log"
test -f "$WORKSPACE/stderr.log"
test -f "$WORKSPACE/manifest.json"
grep -q "fake claude artifact" "$WORKSPACE/result.md"
grep -q "fake claude artifact" "$WORKSPACE/stdout.log"
grep -q "fake claude stderr" "$WORKSPACE/stderr.log"
grep -q '"adapter": "claude"' "$WORKSPACE/manifest.json"
grep -q '"toolProfile": "observe"' "$WORKSPACE/manifest.json"
grep -q '"exitCode": 0' "$WORKSPACE/manifest.json"

jq -s -e --arg taskId "$TASK_ID" '
  [.[] | select(.taskId == $taskId and .schema == "woventeam.task_event.v0.1" and .status == "running")] | length == 1
' "$TASK_LEDGER" >/dev/null
jq -s -e --arg taskId "$TASK_ID" '
  [.[] | select(.taskId == $taskId and .schema == "woventeam.task_event.v0.1" and .status == "complete")] | length == 1
' "$TASK_LEDGER" >/dev/null

grep -q '"messageType":"task.status"' "$ROOM_LOG"
grep -q '"messageType":"task.result"' "$ROOM_LOG"
grep -q 'claude adapter exitCode=0' "$ROOM_LOG"

echo "wt-cli-artifact-adapter integration test passed"
