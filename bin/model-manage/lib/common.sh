#!/usr/bin/env bash

WT_DRY_RUN="${WT_DRY_RUN:-0}"
WT_TIMEOUT_SECONDS="${WT_TIMEOUT_SECONDS:-900}"
WT_BACKUP_ROOT="${WT_BACKUP_ROOT:-/woventeam/backups}"
WT_LOG_ROOT="${WT_LOG_ROOT:-/woventeam/logs}"
WT_RUN_TS="${WT_RUN_TS:-$(date +%Y%m%d-%H%M%S)}"
WT_LOG_FILE="${WT_LOG_FILE:-$WT_LOG_ROOT/model-manage-$WT_RUN_TS.log}"
WT_AUDIT_ROOT="${WT_AUDIT_ROOT:-/woventeam/docs/audit}"

init_common() {
  mkdir -p "$WT_LOG_ROOT"
  touch "$WT_LOG_FILE"
  WT_VERIFY_FAILURES=0
  log "model-manage run started dry_run=$WT_DRY_RUN"
}

log() {
  local message="$*"
  printf '[%s] %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$message" | tee -a "$WT_LOG_FILE"
}

warn() {
  log "WARN: $*"
}

die() {
  log "ERROR: $*"
  exit 1
}

run_action() {
  if [[ "$WT_DRY_RUN" == "1" ]]; then
    log "DRY-RUN: $*"
    return 0
  fi
  log "RUN: $*"
  "$@"
}

confirm_or_die() {
  local prompt="$1"
  if [[ "$WT_DRY_RUN" == "1" ]]; then
    log "DRY-RUN auto-confirmed prompt: $prompt"
    return 0
  fi
  printf '%s [type YES within %ss]: ' "$prompt" "$WT_TIMEOUT_SECONDS" >&2
  local answer=""
  if ! IFS= read -r -t "$WT_TIMEOUT_SECONDS" answer; then
    die "confirmation timed out"
  fi
  [[ "$answer" == "YES" ]] || die "operator confirmation denied"
}

blocking_prompt() {
  local prompt="$1"
  if [[ "$WT_DRY_RUN" == "1" ]]; then
    log "DRY-RUN blocking prompt: $prompt"
    return 0
  fi
  printf '%s\nPress ENTER within %ss when complete: ' "$prompt" "$WT_TIMEOUT_SECONDS" >&2
  IFS= read -r -t "$WT_TIMEOUT_SECONDS" _ || die "blocking prompt timed out"
}

create_backup_dir() {
  WT_BACKUP_DIR="$WT_BACKUP_ROOT/model-manage-$WT_RUN_TS"
  run_action mkdir -p "$WT_BACKUP_DIR"
}

backup_file() {
  local path="$1"
  [[ -e "$path" ]] || die "required backup source missing: $path"
  run_action cp "$path" "$WT_BACKUP_DIR/$(basename "$path").bak"
}

backup_file_if_exists() {
  local path="$1"
  if [[ -e "$path" ]]; then
    run_action cp "$path" "$WT_BACKUP_DIR/$(basename "$path").bak"
  else
    warn "optional backup source missing: $path"
  fi
}

write_file_from_stdin() {
  local path="$1"
  if [[ "$WT_DRY_RUN" == "1" ]]; then
    log "DRY-RUN: would write $path"
    cat >/dev/null
  else
    mkdir -p "$(dirname "$path")"
    cat >"$path"
    log "WROTE: $path"
  fi
}

final_notification_note() {
  local model="$1"
  log "Slack final-notification hook reached for model=$model"
  log "Post-change notification is intentionally surfaced as an operator/integration step rather than silently sent by this scaffold."
}
