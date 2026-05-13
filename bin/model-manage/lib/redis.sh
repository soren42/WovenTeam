#!/usr/bin/env bash

cleanup_model_redis() {
  local model="$1"
  command -v redis-cli >/dev/null 2>&1 || {
    warn "redis-cli unavailable; skipping Redis cleanup"
    return 0
  }

  local patterns=("*${model}*" "*ds_*")
  for pattern in "${patterns[@]}"; do
    local keys
    keys="$(redis-cli --raw KEYS "$pattern" 2>/dev/null || true)"
    if [[ -z "$keys" ]]; then
      log "redis cleanup pattern=$pattern no keys"
      continue
    fi
    while IFS= read -r key; do
      [[ -n "$key" ]] || continue
      run_action redis-cli DEL "$key"
    done <<<"$keys"
  done
  run_action redis-cli BGSAVE
}
