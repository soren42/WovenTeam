#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LIB="$ROOT/bin/model-manage/lib"

# shellcheck source=lib/common.sh
source "$LIB/common.sh"
# shellcheck source=lib/config.sh
source "$LIB/config.sh"
# shellcheck source=lib/inventory.sh
source "$LIB/inventory.sh"
# shellcheck source=lib/credentials.sh
source "$LIB/credentials.sh"
# shellcheck source=lib/redis.sh
source "$LIB/redis.sh"
# shellcheck source=lib/sqlite.sh
source "$LIB/sqlite.sh"
# shellcheck source=lib/files.sh
source "$LIB/files.sh"
# shellcheck source=lib/ownership.sh
source "$LIB/ownership.sh"
# shellcheck source=lib/slack.sh
source "$LIB/slack.sh"
# shellcheck source=lib/docs.sh
source "$LIB/docs.sh"
# shellcheck source=lib/network.sh
source "$LIB/network.sh"
# shellcheck source=lib/verify.sh
source "$LIB/verify.sh"
# shellcheck source=lib/audit.sh
source "$LIB/audit.sh"

usage() {
  cat <<'EOF'
Usage:
  wt-model-manage.sh [--dry-run] remove <model_name>
  wt-model-manage.sh [--dry-run] add <model_name>
  wt-model-manage.sh [--dry-run] rotate-keys
  wt-model-manage.sh [--dry-run] rotate-webhooks
  wt-model-manage.sh [--dry-run] audit [model_name]
  wt-model-manage.sh [--dry-run] verify [model_name]

Notes:
  - Destructive paths require explicit operator confirmation.
  - --dry-run logs intended actions without mutating files or services.
  - Live model registry defaults to /woventeam/config/models.json.
EOF
}

parse_global_flags() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --dry-run)
        WT_DRY_RUN=1
        shift
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        break
        ;;
    esac
  done
  WT_ARGS=("$@")
}

cmd_remove() {
  local model="${1:-}"
  require_model "$model"
  confirm_or_die "Remove model '$model' and execute the managed cleanup workflow?"

  create_backup_dir
  backup_file "$WT_MODELS_JSON"
  backup_file_if_exists "$WT_DB_PATH"

  inventory_model "$model"
  remove_target_credentials "$model"
  cleanup_model_redis "$model"
  cleanup_model_sqlite "$model"
  update_model_files "$model"
  transfer_coordination_ownership "$model"
  slack_cleanup_for_model "$model"
  run_network_checks "$model"
  mark_model_removed "$model"
  verify_model_removal "$model"
  generate_audit_report "$model" "remove"
  final_notification_note "$model"
  verify_assert_clean
}

cmd_add() {
  local model="${1:-}"
  [[ -n "$model" ]] || die "add requires a model name"
  log "add workflow scaffold invoked for '$model'"
  log "Current implementation records the action path but intentionally does not auto-onboard models yet."
  log "Expected next work: prompt for vendor metadata, create registry entry, assign trust tier, verify credentials."
}

cmd_rotate_keys() {
  confirm_or_die "Begin guided vendor credential rotation workflow?"
  create_backup_dir
  rotate_all_vendor_keys
  generate_audit_report "all-vendors" "rotate-keys"
}

cmd_audit() {
  local model="${1:-deepseek}"
  require_model "$model"
  inventory_model "$model"
  audit_model_registry "$model"
  generate_audit_report "$model" "audit"
}

cmd_verify() {
  local model="${1:-deepseek}"
  require_model "$model"
  verify_model_removal "$model"
  verify_assert_clean
}

cmd_rotate_webhooks() {
  confirm_or_die "Begin Slack webhook rotation workflow?"
  slack_rotate_webhooks
}

main() {
  parse_global_flags "$@"
  init_common
  set -- "${WT_ARGS[@]}"

  local subcommand="${1:-}"
  shift || true

  case "$subcommand" in
    remove) cmd_remove "$@" ;;
    add) cmd_add "$@" ;;
    rotate-keys) cmd_rotate_keys "$@" ;;
    rotate-webhooks) cmd_rotate_webhooks "$@" ;;
    audit) cmd_audit "$@" ;;
    verify) cmd_verify "$@" ;;
    ""|-h|--help) usage ;;
    *) die "unknown subcommand: $subcommand" ;;
  esac
}

main "$@"
