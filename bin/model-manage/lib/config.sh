#!/usr/bin/env bash

WT_MODELS_JSON="${WT_MODELS_JSON:-/woventeam/config/models.json}"
WT_DB_PATH="${WT_DB_PATH:-/woventeam/woventeam.db}"
WT_REPO_ROOT="${WT_REPO_ROOT:-/woventeam/repos/WovenTeam}"
WT_SCAN_ROOT="${WT_SCAN_ROOT:-/woventeam}"

require_jq() {
  command -v jq >/dev/null 2>&1 || die "jq is required"
}

require_model_registry() {
  [[ -f "$WT_MODELS_JSON" ]] || die "model registry missing: $WT_MODELS_JSON"
  require_jq
  jq empty "$WT_MODELS_JSON" >/dev/null
}

require_model() {
  local model="${1:-}"
  [[ -n "$model" ]] || die "model name is required"
  require_model_registry
  jq -e --arg model "$model" '.models[$model] != null' "$WT_MODELS_JSON" >/dev/null \
    || die "model not found in registry: $model"
}

model_field() {
  local model="$1"
  local expr="$2"
  jq -r --arg model "$model" ".models[\$model].$expr // empty" "$WT_MODELS_JSON"
}

audit_model_registry() {
  local model="$1"
  log "registry entry for $model:"
  jq --arg model "$model" '.models[$model]' "$WT_MODELS_JSON" | tee -a "$WT_LOG_FILE"
}

mark_model_removed() {
  local model="$1"
  local tmp
  tmp="$(mktemp)"
  jq --arg model "$model" --arg today "$(date -u +%Y-%m-%d)" '
    .models[$model].status = "removed"
    | .models[$model].tier = 3
    | .models[$model].removal_date = (.models[$model].removal_date // $today)
    | .blocked_vendors = ((.blocked_vendors // []) + [(.models[$model].vendor // $model)] | unique)
  ' "$WT_MODELS_JSON" >"$tmp"
  if [[ "$WT_DRY_RUN" == "1" ]]; then
    log "DRY-RUN: would update $WT_MODELS_JSON to mark $model removed"
    rm -f "$tmp"
  else
    mv "$tmp" "$WT_MODELS_JSON"
    log "updated model registry for $model"
  fi
}
