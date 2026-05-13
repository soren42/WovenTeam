#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

mkdir -p "$TMPDIR/config" "$TMPDIR/logs" "$TMPDIR/audit" "$TMPDIR/backups" "$TMPDIR/scan"
cp /woventeam/config/models.json "$TMPDIR/config/models.json"
printf 'deepseek reference for inventory\n' > "$TMPDIR/scan/reference.txt"

COMMON_ENV=(
  WT_MODELS_JSON="$TMPDIR/config/models.json"
  WT_LOG_ROOT="$TMPDIR/logs"
  WT_AUDIT_ROOT="$TMPDIR/audit"
  WT_BACKUP_ROOT="$TMPDIR/backups"
  WT_DB_PATH="$TMPDIR/missing.db"
  WT_SCAN_ROOT="$TMPDIR/scan"
)

env "${COMMON_ENV[@]}" "$ROOT/bin/model-manage/wt-model-manage.sh" --dry-run audit deepseek > "$TMPDIR/audit.out"
grep -q 'inventory scan started for deepseek' "$TMPDIR/audit.out"
grep -q 'registry entry for deepseek' "$TMPDIR/audit.out" || \
  grep -q 'registry status removed for deepseek' "$TMPDIR/audit.out" || \
  { echo "audit output missing expected registry trace" >&2; cat "$TMPDIR/audit.out" >&2; exit 1; }

env "${COMMON_ENV[@]}" "$ROOT/bin/model-manage/wt-model-manage.sh" --dry-run verify deepseek > "$TMPDIR/verify.out"
grep -q 'PASS registry status removed for deepseek' "$TMPDIR/verify.out"
grep -q 'verification clean' "$TMPDIR/verify.out"

before="$(sha256sum "$TMPDIR/config/models.json" | awk '{print $1}')"
env "${COMMON_ENV[@]}" "$ROOT/bin/model-manage/wt-model-manage.sh" --dry-run remove deepseek > "$TMPDIR/remove.out"
after="$(sha256sum "$TMPDIR/config/models.json" | awk '{print $1}')"
test "$before" = "$after"
grep -q 'DRY-RUN auto-confirmed prompt' "$TMPDIR/remove.out"
grep -q 'DRY-RUN: would update' "$TMPDIR/remove.out"

# Non-dry-run pass: use a synthetic test model so the real registry never changes.
mkdir -p "$TMPDIR/live/config" "$TMPDIR/live/scan"
cat > "$TMPDIR/live/config/models.json" <<'JSON'
{
  "schema_version": "1.0",
  "models": {
    "claude":   { "vendor": "anthropic", "tier": 1, "status": "active",  "api_key_env_var": "ANTHROPIC_API_KEY", "workgroup": "dashboard" },
    "testmodel":{ "vendor": "fakevendor","tier": 2, "status": "active",  "api_key_env_var": "TESTMODEL_API_KEY", "workgroup": "coordination-api" }
  },
  "blocked_vendors": [],
  "tier_definitions": { "1": "lvl1", "2": "lvl2", "3": "lvl3" }
}
JSON
cat > "$TMPDIR/live/config/routing.json" <<'JSON'
{
  "preferred_vendors": ["anthropic","fakevendor"],
  "blocked_models": []
}
JSON
printf 'TestModel reference for case-insensitive inventory\n' > "$TMPDIR/live/scan/casetest.txt"

touch "$TMPDIR/live/fakeenv"
printf 'export TESTMODEL_API_KEY=fake\n' > "$TMPDIR/live/fakeenv"

LIVE_ENV=(
  WT_MODELS_JSON="$TMPDIR/live/config/models.json"
  WT_LOG_ROOT="$TMPDIR/logs"
  WT_AUDIT_ROOT="$TMPDIR/audit"
  WT_BACKUP_ROOT="$TMPDIR/backups"
  WT_DB_PATH="$TMPDIR/missing.db"
  WT_SCAN_ROOT="$TMPDIR/live/scan"
  WT_ENV_FILES="$TMPDIR/live/fakeenv"
)

# Auto-confirm via piped input: YES for confirm_or_die, then ENTER for the
# slack blocking prompt. Use a here-doc rather than `yes` to avoid SIGPIPE.
env "${LIVE_ENV[@]}" "$ROOT/bin/model-manage/wt-model-manage.sh" remove testmodel \
  > "$TMPDIR/live-remove.out" 2>&1 <<'INPUT'
YES

INPUT

# Inventory should have matched mixed-case "TestModel" because grep -i is used.
grep -q 'casetest.txt' "$TMPDIR/live-remove.out" || {
  echo "FAIL: case-insensitive inventory did not match TestModel" >&2
  cat "$TMPDIR/live-remove.out" >&2
  exit 1
}

grep -Eq 'TESTMODEL_API_KEY' "$TMPDIR/live/fakeenv" && {
  echo "FAIL: TESTMODEL_API_KEY still present in env file after scrub" >&2
  cat "$TMPDIR/live/fakeenv" >&2
  exit 1
} || true

# Registry must show testmodel as removed with vendor blocked.
jq -e '.models.testmodel.status == "removed"' "$TMPDIR/live/config/models.json" >/dev/null
jq -e '.blocked_vendors | index("fakevendor") != null' "$TMPDIR/live/config/models.json" >/dev/null

# routing.json must have been scrubbed.
jq -e '.preferred_vendors | index("fakevendor") == null' "$TMPDIR/live/config/routing.json" >/dev/null
jq -e '.blocked_models | index("testmodel") != null' "$TMPDIR/live/config/routing.json" >/dev/null

# Coordination-API ownership-transfer artifact must exist (workgroup matched).
ls "$TMPDIR/audit"/coordination-api-ownership-*.md >/dev/null

# Audit report must have been rendered from the template.
ls "$TMPDIR/audit"/model-manage-remove-testmodel-*.md >/dev/null
report="$(ls "$TMPDIR/audit"/model-manage-remove-testmodel-*.md | head -1)"
grep -q 'Ownership transfer' "$report"
grep -q 'fakevendor' "$report"

echo "model-manage integration test passed (dry-run + live sandbox)"
