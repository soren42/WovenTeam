#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

ROOM_LOG="$TMPDIR/phase0-room.jsonl"
TASK_LEDGER="$TMPDIR/task-packages.jsonl"
RUNTIME_ROOT="$TMPDIR/runtime/tasks"
TASK_ID="task_codex_adapter_001"

cat > "$TMPDIR/fake-codex" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
out=""
workspace=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --output-last-message) out="$2"; shift 2 ;;
    --cd) workspace="$2"; shift 2 ;;
    *) shift ;;
  esac
done
test -n "$out"
test -n "$workspace"
printf 'fake codex result in %s\n' "$workspace" > "$out"
printf '{"artifact":"created by fake codex"}\n' > "$workspace/artifact.json"
echo "fake codex stdout"
echo "fake codex stderr" >&2
SH
chmod +x "$TMPDIR/fake-codex"

cat > "$TASK_LEDGER" <<JSON
{"schema":"woventeam.task_package.v0.1","taskId":"$TASK_ID","initiativeId":"init_codex_adapter","createdBy":"ceo","assignedRole":"backend_dev","assignedAgent":"chatgpt","modelId":"openai/gpt-5.3-codex","priority":"normal","status":"queued","title":"Codex adapter smoke","body":"Create a fake artifact in the isolated workspace.","task":{"title":"Codex adapter smoke","body":"Create a fake artifact in the isolated workspace.","deliverables":[]},"contextRefs":[],"acceptanceCriteria":["Fake Codex result is captured."],"toolPolicy":{"profile":"repo_branch","filesystem":"workspace_write","network":"none","system":"none","git":"branch_only"},"budget":{"timeoutSeconds":10,"maxOutputBytes":1048576,"maxCostUsd":0.01},"dependencies":[],"createdAtUnixMs":1778915000000}
JSON

WT_ROOM_LOG_PATH="$ROOM_LOG" \
WT_TASK_LEDGER_PATH="$TASK_LEDGER" \
WT_ENABLE_CODEX_ADAPTER=1 \
WT_RUNTIME_ROOT_PATH="$RUNTIME_ROOT" \
WT_GPT_COMMAND="$TMPDIR/fake-codex" \
  "$ROOT/build/wt-agent" --agent chatgpt --once > "$TMPDIR/agent.out"

WORKSPACE="$RUNTIME_ROOT/$TASK_ID"
test -f "$WORKSPACE/prompt.md"
test -f "$WORKSPACE/result.md"
test -f "$WORKSPACE/stdout.log"
test -f "$WORKSPACE/stderr.log"
test -f "$WORKSPACE/manifest.json"
grep -q "fake codex result" "$WORKSPACE/result.md"
grep -q "fake codex stdout" "$WORKSPACE/stdout.log"
grep -q "fake codex stderr" "$WORKSPACE/stderr.log"
grep -q '"adapter": "codex"' "$WORKSPACE/manifest.json"
grep -q '"exitCode": 0' "$WORKSPACE/manifest.json"

jq -s -e --arg taskId "$TASK_ID" '
  [.[] | select(.taskId == $taskId and .schema == "woventeam.task_event.v0.1" and .status == "running")] | length == 1
' "$TASK_LEDGER" >/dev/null
jq -s -e --arg taskId "$TASK_ID" '
  [.[] | select(.taskId == $taskId and .schema == "woventeam.task_event.v0.1" and .status == "complete")] | length == 1
' "$TASK_LEDGER" >/dev/null

grep -q '"messageType":"task.status"' "$ROOM_LOG"
grep -q '"messageType":"task.result"' "$ROOM_LOG"
grep -q 'codex adapter exitCode=0' "$ROOM_LOG"

echo "wt-codex-adapter integration test passed"
