#!/usr/bin/env bash
# Phase 3 Sprint 3: deliverables pipeline integration test.
#
# Covers:
#   - copy mode ships an accepted artifact + records a deliverable manifest
#   - tarball mode produces a .tar.gz under the deliverable root
#   - secret scan records a hit (without blocking) on copy/tarball modes
#   - branch + pr modes refuse to ship a planted token (secret_scan_block)
#   - branch mode commits and pushes to a local fixture repo
#   - supersede chains carry the predecessor's deliverableId
#   - audit export includes deliverables[] + secretScans[]
#
# PR mode is exercised against a local fixture repo; we intercept the gh CLI
# with a stub on PATH so the test does not require a real GitHub remote.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TMPDIR="$(mktemp -d)"
PORT="${WT_TEST_PORT:-$((28000 + RANDOM % 1000))}"
CONFIG="$TMPDIR/woventeam.conf"
ROOM_LOG="$TMPDIR/data/phase0-room.jsonl"
TASK_LEDGER="$TMPDIR/data/task-packages.jsonl"
PROJECTION="$TMPDIR/data/task-projection.sqlite"
RUNTIME="$TMPDIR/runtime/tasks"
DELIVERABLE_ROOT="$TMPDIR/data/deliverables"
SERVER_LOG="$TMPDIR/wt-roomd.log"
ROOM_URL="http://127.0.0.1:$PORT"
PATTERNS_FILE="$TMPDIR/secret-patterns.txt"
FIXTURE_REPO="$TMPDIR/fixture-repo"
FIXTURE_REMOTE="$TMPDIR/fixture-remote.git"
GH_STUB_DIR="$TMPDIR/gh-stub"
GH_STUB_LOG="$TMPDIR/gh-stub.log"

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]]; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf "$TMPDIR"
}
trap cleanup EXIT

mkdir -p "$TMPDIR/data" "$RUNTIME" "$DELIVERABLE_ROOT"

# Conservative patterns file: github-pat shape and a generic bearer token.
cat >"$PATTERNS_FILE" <<'PATTERNS'
github-pat=ghp_[A-Za-z0-9]{36}
generic-bearer=(api[_-]?key|secret|token)[[:space:]]*[=:][[:space:]]*['"]?[A-Za-z0-9_+/=-]{24,}
PATTERNS

# gh stub: when called, log the args + exit 0. Branches + PR mode never
# actually reach github.com. The log path is baked into the stub at write
# time (rather than read from env) because the daemon doesn't inherit
# our shell's GH_STUB_LOG var.
mkdir -p "$GH_STUB_DIR"
cat >"$GH_STUB_DIR/gh" <<STUB
#!/usr/bin/env bash
echo "gh-stub \$*" >> "$GH_STUB_LOG"
exit 0
STUB
chmod +x "$GH_STUB_DIR/gh"

# Set up a local bare remote + working clone so branch/pr modes can push.
git init --quiet --bare "$FIXTURE_REMOTE"
git -c init.defaultBranch=main init --quiet "$FIXTURE_REPO"
(
  cd "$FIXTURE_REPO"
  git config user.email "wt-test@example.com"
  git config user.name "wt test"
  echo "fixture" > README.md
  git add README.md
  git commit --quiet -m "init"
  git branch -M main
  git remote add origin "$FIXTURE_REMOTE"
  git push --quiet -u origin main
)

cat >"$CONFIG" <<CFG
roomName=deliverables
roomLogPath=$ROOM_LOG
taskLedgerPath=$TASK_LEDGER
taskProjectionDbPath=$PROJECTION
httpBindAddress=127.0.0.1
httpPort=$PORT
fsyncEachMessage=0
roleRoutingEnabled=1
maxActiveTasksPerAgent=8
maxSubtasksPerParent=8
maxTasksPerInitiative=20
runtimeRootPath=$RUNTIME
deliverableRoot=$DELIVERABLE_ROOT
deliverableDefaultMode=copy
deliverableBranchPrefix=deliverables
secretScanPatternsFile=$PATTERNS_FILE
CFG

# Launch the daemon with the gh stub on PATH so `gh pr create` from inside
# wt_deliverable's PR mode hits our stub (which logs + exit 0) instead of
# the real gh CLI talking to a remote that doesn't exist.
PATH="$GH_STUB_DIR:$PATH" "$ROOT/build/wt-roomd" --config "$CONFIG" >"$SERVER_LOG" 2>&1 &
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

initiative="init_deliverables"

create_and_accept_task() {
  # $1 = title, $2 = result file content
  local response taskId
  response="$(
    WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" create \
      --title "$1" \
      --body  "deliverables test task" \
      --role  backend_dev \
      --agent chatgpt \
      --initiative "$initiative" \
      --max-tokens 100
  )"
  taskId="$(jq -r '.taskId' <<<"$response")"
  test "$taskId" != "null"
  mkdir -p "$RUNTIME/$taskId"
  printf '%s' "$2" > "$RUNTIME/$taskId/result.md"
  # promote -> accepted so the projection latches accepted_artifact_path
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" artifact promote "$taskId" \
    --path result.md --reviewer ceo --notes "ship it" >/dev/null
  echo "$taskId"
}

# --- Case 1: copy mode ships a clean artifact ---
task_copy="$(create_and_accept_task "clean copy" "Clean deliverable contents, no secrets here.")"
ship_copy="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" artifact ship "$task_copy" \
    --mode copy --reviewer ceo
)"
echo "$ship_copy" | jq -e '.ok == true and .packagingMode == "copy" and .scan.matched == false' >/dev/null
deliverable_path_copy="$(jq -r '.deliverablePath' <<<"$ship_copy")"
sha_copy="$(jq -r '.sha256' <<<"$ship_copy")"
test -f "$deliverable_path_copy"
# Verify checksum matches an independent computation.
local_sha="$(sha256sum "$RUNTIME/$task_copy/result.md" | awk '{print $1}')"
[[ "$sha_copy" == "$local_sha" ]] || { echo "copy sha mismatch: $sha_copy vs $local_sha"; exit 1; }

# --- Case 2: tarball mode ---
task_tar="$(create_and_accept_task "clean tarball" "Tarball contents.")"
ship_tar="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" artifact ship "$task_tar" \
    --mode tarball --reviewer ceo
)"
echo "$ship_tar" | jq -e '.ok == true and .packagingMode == "tarball"' >/dev/null
tarball_path="$(jq -r '.deliverablePath' <<<"$ship_tar")"
test -f "$tarball_path"
tar tzf "$tarball_path" | grep -q "result.md"

# --- Case 3: secret scan hit on copy mode RECORDS but does NOT block ---
task_recorded="$(create_and_accept_task "planted token but copy mode" \
  "Generic config: api_key = ABCDEFGHIJKLMNOPQRSTUVWXYZ012345")"
ship_recorded="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" artifact ship "$task_recorded" \
    --mode copy --reviewer ceo
)"
echo "$ship_recorded" | jq -e '.ok == true and .scan.matched == true and .scan.hitCount >= 1' >/dev/null

# --- Case 4: secret scan hit on branch mode REFUSES ---
task_blocked="$(create_and_accept_task "planted token, branch mode" \
  "Token: ghp_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")"
set +e
ship_blocked="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" artifact ship "$task_blocked" \
    --mode branch --repo "$FIXTURE_REPO" 2>&1
)"
blocked_rc=$?
set -e
# curl --fail returns 22 on 4xx; check the body if we captured it via the wrapper.
[[ $blocked_rc -ne 0 ]] || { echo "branch mode did not refuse on planted token"; exit 1; }
# Confirm the secret_scan record landed in the ledger.
grep -q '"schema":"woventeam.secret_scan.v0.1"' "$TASK_LEDGER"
grep -q '"matched":true' "$TASK_LEDGER"

# --- Case 5: branch mode ships clean to fixture repo ---
task_branch="$(create_and_accept_task "clean for branch" "Clean branch contents.")"
ship_branch="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" artifact ship "$task_branch" \
    --mode branch --repo "$FIXTURE_REPO" --reviewer ceo
)"
echo "$ship_branch" | jq -e '.ok == true and .packagingMode == "branch"' >/dev/null
# The branch should now exist on the remote.
(
  cd "$FIXTURE_REPO"
  git fetch --quiet origin "deliverables/$initiative"
  git rev-parse --quiet --verify "origin/deliverables/$initiative" >/dev/null
)

# --- Case 6: pr mode requires --yes; without it, refused ---
task_pr="$(create_and_accept_task "clean for pr" "Clean PR contents.")"
set +e
ship_pr_no_yes="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" artifact ship "$task_pr" \
    --mode pr --repo "$FIXTURE_REPO" --reviewer ceo 2>&1
)"
no_yes_rc=$?
set -e
[[ $no_yes_rc -ne 0 ]] || { echo "pr mode succeeded without --yes"; exit 1; }
# Now with --yes the daemon (already launched with the gh stub on its PATH)
# will succeed.
ship_pr_yes="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" artifact ship "$task_pr" \
    --mode pr --repo "$FIXTURE_REPO" --reviewer ceo --yes \
    --pr-title "Test PR" --pr-body "from wt-deliverables.sh"
)"
echo "$ship_pr_yes" | jq -e '.ok == true and .packagingMode == "pull-request"' >/dev/null
# Verify the stub was actually invoked.
grep -q 'gh-stub pr create' "$GH_STUB_LOG"

# --- Case 7: supersede chain ---
task_super="$(create_and_accept_task "supersede source" "v1 contents.")"
ship_v1="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" artifact ship "$task_super" \
    --mode copy --reviewer ceo
)"
v1_id="$(jq -r '.deliverableId' <<<"$ship_v1")"
# Overwrite the source and re-ship superseding v1.
printf 'v2 contents revised.' > "$RUNTIME/$task_super/result.md"
ship_v2="$(
  WT_ROOM_URL="$ROOM_URL" "$ROOT/bin/wt-task" artifact ship "$task_super" \
    --mode copy --reviewer ceo --supersedes "$v1_id"
)"
v2_id="$(jq -r '.deliverableId' <<<"$ship_v2")"
test -n "$v2_id"
# v2 ledger record should carry "supersedes":"<v1_id>".
grep -q "\"supersedes\":\"$v1_id\"" "$TASK_LEDGER"

# --- Case 8: audit export includes deliverables[] + secretScans[] ---
audit="$(curl -fsS "$ROOM_URL/api/initiative-audit?initiativeId=$initiative")"
echo "$audit" | jq -e '.deliverables | length >= 6' >/dev/null   # 6 successful ships above
echo "$audit" | jq -e '.secretScans  | length >= 1' >/dev/null
# Each deliverable carries a sha256 and a packagingMode.
echo "$audit" | jq -e '
  .deliverables | all(.sha256 | test("^[0-9a-f]{64}$"))
  and all(.packagingMode | test("^(copy|tarball|branch|pull-request)$"))
' >/dev/null

echo "wt-deliverables integration test passed"
