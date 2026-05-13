#!/usr/bin/env bash

verify_fail() {
  WT_VERIFY_FAILURES=$((WT_VERIFY_FAILURES + 1))
  warn "FAIL $*"
}

verify_assert_clean() {
  if [[ "${WT_VERIFY_FAILURES:-0}" -gt 0 ]]; then
    die "verification failed with ${WT_VERIFY_FAILURES} issue(s)"
  fi
  log "verification clean (0 failures)"
}

verify_model_removal() {
  local model="$1"
  require_model "$model"
  log "verification started for $model"

  if jq -e --arg model "$model" '.models[$model].status == "removed"' "$WT_MODELS_JSON" >/dev/null; then
    log "PASS registry status removed for $model"
  else
    verify_fail "registry status is not removed for $model"
  fi

  local vendor
  vendor="$(model_field "$model" vendor)"
  if jq -e --arg vendor "$vendor" '(.blocked_vendors // []) | index($vendor) != null' "$WT_MODELS_JSON" >/dev/null; then
    log "PASS blocked vendor recorded for $model"
  else
    verify_fail "blocked vendor missing for $model (vendor=$vendor)"
  fi

  if command -v redis-cli >/dev/null 2>&1; then
    local residual residual_ds
    residual="$(redis-cli --raw KEYS "*${model}*" 2>/dev/null | grep -c . || true)"
    residual_ds="$(redis-cli --raw KEYS "*ds_*" 2>/dev/null | grep -c . || true)"
    if [[ "$residual" -eq 0 ]]; then
      log "PASS redis residual keys for $model: 0"
    else
      verify_fail "redis residual keys for $model: $residual"
    fi
    if [[ "$residual_ds" -eq 0 ]]; then
      log "PASS redis residual ds-pattern keys: 0"
    else
      verify_fail "redis residual ds-pattern keys: $residual_ds"
    fi
  else
    warn "redis-cli unavailable during verification"
  fi

  local env_var
  env_var="$(model_field "$model" api_key_env_var)"
  if [[ -n "$env_var" ]]; then
    local hits=0
    local file
    for file in /etc/environment /home/woven/.bashrc /home/woven/.zshrc /home/woven/.env /woventeam/.env; do
      [[ -f "$file" ]] || continue
      if grep -Eq "^(export[[:space:]]+)?${env_var}=" "$file"; then
        hits=$((hits + 1))
        verify_fail "residual ${env_var} reference in ${file}"
      fi
    done
    if [[ "$hits" -eq 0 ]]; then
      log "PASS no residual ${env_var} references in standard env files"
    fi
  fi
}
