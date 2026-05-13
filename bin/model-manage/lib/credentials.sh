#!/usr/bin/env bash

remove_target_credentials() {
  local model="$1"
  local env_var
  env_var="$(model_field "$model" api_key_env_var)"
  [[ -n "$env_var" ]] || {
    warn "no API env var registered for $model"
    return 0
  }

  log "credential cleanup target env_var=$env_var"
  local files
  if [[ -n "${WT_ENV_FILES:-}" ]]; then
    IFS=':' read -r -a files <<<"$WT_ENV_FILES"
  else
    files=(
      "/etc/environment"
      "/home/woven/.bashrc"
      "/home/woven/.zshrc"
      "/home/woven/.env"
      "/woventeam/.env"
    )
  fi
  for file in "${files[@]}"; do
    [[ -f "$file" ]] || continue
    if grep -Eq "^(export[[:space:]]+)?${env_var}=" "$file" 2>/dev/null; then
      if [[ "$WT_DRY_RUN" != "1" && ! -w "$file" ]]; then
        warn "cannot write $file (skipping scrub of $env_var; rerun as a user with write access)"
        continue
      fi
      remove_env_var_line "$file" "$env_var"
    else
      log "no $env_var entry in $file"
    fi
  done
}

remove_env_var_line() {
  local file="$1"
  local env_var="$2"
  local tmp
  tmp="$(mktemp)"
  grep -Ev "^(export[[:space:]]+)?${env_var}=" "$file" >"$tmp" || true
  if [[ "$WT_DRY_RUN" == "1" ]]; then
    log "DRY-RUN: would scrub $env_var from $file"
    rm -f "$tmp"
  else
    cp "$file" "$file.pre-model-manage-$WT_RUN_TS.bak"
    mv "$tmp" "$file"
    log "scrubbed $env_var from $file"
  fi
}

rotate_all_vendor_keys() {
  require_model_registry
  while IFS=$'\t' read -r model vendor env_var status; do
    [[ "$status" == "active" ]] || continue
    blocking_prompt "MANUAL ACTION REQUIRED: rotate credentials for $vendor ($model), then stage the new value for $env_var in the approved secret store."
    log "rotation prompt completed for model=$model env_var=$env_var"
  done < <(jq -r '.models | to_entries[] | [.key, .value.vendor, (.value.api_key_env_var // ""), .value.status] | @tsv' "$WT_MODELS_JSON")
}
