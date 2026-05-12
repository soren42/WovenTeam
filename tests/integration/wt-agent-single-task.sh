#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

DB="$TMPDIR/woventeam.db"
TASK_ID="task_test_001"
MISSING_TASK_ID="task_missing_001"
LOUD_TASK_ID="task_loud_001"

sqlite3 "$DB" <<SQL
CREATE TABLE tasks (
  id TEXT PRIMARY KEY,
  initiative TEXT,
  assigned_to TEXT,
  status TEXT NOT NULL,
  created_at INTEGER DEFAULT (strftime('%s','now'))
);
INSERT INTO tasks (id, initiative, assigned_to, status) VALUES ('$TASK_ID', 'phase0-spike', 'tester', 'pending');
SQL

cat > "$TMPDIR/task.json" <<JSON
{
  "id": "$TASK_ID",
  "ts": 1778540400,
  "from": "ceo",
  "type": "task",
  "initiative": "phase0-spike",
  "payload": {
    "assigned_to": "tester",
    "command": "printf phase0-c-agent-ok"
  }
}
JSON

WOVENTEAM_DB="$DB" "$ROOT/bin/wt-agent" --agent tester --task-json "$TMPDIR/task.json" --no-redis --no-slack > "$TMPDIR/out.txt"

STATUS="$(sqlite3 "$DB" "SELECT status FROM tasks WHERE id = '$TASK_ID';")"
OUTPUT="$(cat "$TMPDIR/out.txt")"

test "$STATUS" = "complete"
grep -q "phase0-c-agent-ok" <<<"$OUTPUT"

cat > "$TMPDIR/missing-task.json" <<JSON
{
  "id": "$MISSING_TASK_ID",
  "ts": 1778540400,
  "from": "ceo",
  "type": "task",
  "initiative": "phase0-spike",
  "payload": {
    "assigned_to": "tester",
    "command": "touch '$TMPDIR/should-not-run'"
  }
}
JSON

if WOVENTEAM_DB="$DB" "$ROOT/bin/wt-agent" --agent tester --task-json "$TMPDIR/missing-task.json" --no-redis --no-slack > "$TMPDIR/missing-out.txt" 2>&1; then
  echo "expected missing SQLite task update to fail" >&2
  exit 1
fi
if [[ -e "$TMPDIR/should-not-run" ]]; then
  echo "agent executed a task before durable SQLite state existed" >&2
  exit 1
fi

sqlite3 "$DB" "INSERT INTO tasks (id, initiative, assigned_to, status) VALUES ('$LOUD_TASK_ID', 'phase0-spike', 'tester', 'pending');"
cat > "$TMPDIR/loud-task.json" <<JSON
{
  "id": "$LOUD_TASK_ID",
  "ts": 1778540400,
  "from": "ceo",
  "type": "task",
  "initiative": "phase0-spike",
  "payload": {
    "assigned_to": "tester",
    "command": "yes x | head -c 1200000"
  }
}
JSON

WOVENTEAM_DB="$DB" "$ROOT/bin/wt-agent" --agent tester --task-json "$TMPDIR/loud-task.json" --no-redis --no-slack > "$TMPDIR/loud-out.txt"
LOUD_STATUS="$(sqlite3 "$DB" "SELECT status FROM tasks WHERE id = '$LOUD_TASK_ID';")"
test "$LOUD_STATUS" = "complete"

echo "wt-agent single-task integration test passed"
