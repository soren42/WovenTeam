#!/usr/bin/env bash
# Smoke test: wt-agent must resolve Slack webhook URLs from a config file
# that holds KEY=VALUE pairs, honor WOVENTEAM_SLACK_WEBHOOK_KEY, and
# fall back to the first hooks.slack.com URL when no key matches.
# Back-compat: a bare-URL file must still work.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FIX="$ROOT/build/wt-agent"
[[ -x "$FIX" ]] || { echo "build/wt-agent missing; run 'make build/wt-agent' first" >&2; exit 1; }

TMPD="$(mktemp -d)"
trap 'rm -rf "$TMPD"' EXIT

# Fixtures
cat > "$TMPD/keyed.txt" <<'EOF'
integration=https://hooks.slack.com/services/INT/AAA/BBB
reviews=https://hooks.slack.com/services/REV/CCC/DDD
EOF
echo 'https://hooks.slack.com/services/BARE/EEE/FFF' > "$TMPD/bare.txt"
cat > "$TMPD/reviews-only.txt" <<'EOF'
# webhook config
reviews=https://hooks.slack.com/services/REV/GGG/HHH
EOF
echo 'something=else' > "$TMPD/junk.txt"

# Curl stub captures the URL wt-agent would have hit.
mkdir -p "$TMPD/bin"
cat > "$TMPD/bin/curl" <<'BASH'
#!/usr/bin/env bash
for a in "$@"; do printf '%s\n' "$a"; done >> "${WT_CURL_LOG:?}"
exit 0
BASH
chmod +x "$TMPD/bin/curl"

cat > "$TMPD/task.json" <<'JSON'
{ "id": "t-keyed-001", "ts": 1, "from": "test", "type": "task", "initiative": "smoke-test",
  "payload": { "assigned_to": "claude", "command": "true" } }
JSON
sqlite3 "$TMPD/wt.db" \
  "CREATE TABLE tasks(id TEXT PRIMARY KEY, initiative TEXT, assigned_to TEXT, status TEXT, created_at INTEGER); \
   INSERT INTO tasks VALUES('t-keyed-001','smoke-test','claude','pending',1);"

reset_task() {
  sqlite3 "$TMPD/wt.db" "UPDATE tasks SET status='pending' WHERE id='t-keyed-001';"
}

run_case() {
  local webhook="$1" key="$2"
  local log; log="$(mktemp -p "$TMPD")"
  local env_args=(
    "PATH=$TMPD/bin:$PATH"
    "WT_CURL_LOG=$log"
    "WOVENTEAM_DB=$TMPD/wt.db"
    "WOVENTEAM_SLACK_WEBHOOK_FILE=$webhook"
  )
  [[ -n "$key" ]] && env_args+=("WOVENTEAM_SLACK_WEBHOOK_KEY=$key")
  env "${env_args[@]}" "$FIX" --agent claude --task-json "$TMPD/task.json" --no-redis >/dev/null 2>&1
  reset_task
  grep -E '^https://hooks\.slack\.com/' "$log" | head -1
}

assert_eq() {
  local got="$1" want="$2" label="$3"
  if [[ "$got" != "$want" ]]; then
    echo "FAIL: $label: got '$got', want '$want'" >&2
    exit 1
  fi
}

assert_eq "$(run_case "$TMPD/keyed.txt"        "")"           "https://hooks.slack.com/services/INT/AAA/BBB" "default-key-integration"
assert_eq "$(run_case "$TMPD/keyed.txt"        "reviews")"    "https://hooks.slack.com/services/REV/CCC/DDD" "explicit-key-reviews"
assert_eq "$(run_case "$TMPD/keyed.txt"        "no-such")"    "https://hooks.slack.com/services/INT/AAA/BBB" "missing-key-falls-back-to-first-url"
assert_eq "$(run_case "$TMPD/bare.txt"         "")"           "https://hooks.slack.com/services/BARE/EEE/FFF" "bare-url-back-compat"
assert_eq "$(run_case "$TMPD/reviews-only.txt" "")"           "https://hooks.slack.com/services/REV/GGG/HHH" "comments-skipped-fallback"
assert_eq "$(run_case "$TMPD/junk.txt"         "")"           ""                                              "no-url-no-post"

echo "wt-agent slack-webhook keyed test passed"
