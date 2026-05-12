#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if ! command -v redis-cli >/dev/null 2>&1; then
  echo "redis-cli is required" >&2
  exit 1
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

cat > "$TMPDIR/redis-cli" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
if [[ "$1" != "publish" ]]; then
  echo "unexpected redis-cli command: $*" >&2
  exit 2
fi
sqlite3 "$WOVENTEAM_DB" "SELECT CASE WHEN EXISTS (SELECT 1 FROM tasks WHERE id = '$WTCTL_EXPECTED_TASK_ID' AND status = 'pending') THEN 0 ELSE 1 END;" | grep -q '^0$'
exit 0
SH
chmod +x "$TMPDIR/redis-cli"

DB="$TMPDIR/woventeam.db"
sqlite3 "$DB" <<SQL
CREATE TABLE tasks (
  id TEXT PRIMARY KEY,
  initiative TEXT,
  assigned_to TEXT,
  status TEXT NOT NULL,
  created_at INTEGER DEFAULT (strftime('%s','now'))
);
SQL

cat > "$TMPDIR/uuidgen" <<'SH'
#!/usr/bin/env bash
printf '%s\n' "$WTCTL_EXPECTED_TASK_ID"
SH
chmod +x "$TMPDIR/uuidgen"

WTCTL_EXPECTED_TASK_ID="task_order_001" \
WOVENTEAM_DB="$DB" \
PATH="$TMPDIR:$PATH" \
"$ROOT/bin/wtctl" tester "printf dispatch-order-ok" >/tmp/wtctl-dispatch-order.out

echo "wtctl dispatch order test passed"
