#!/usr/bin/env bash

update_model_files() {
  local model="$1"
  log "file-update phase entered for $model"

  scrub_config_jsons "$model"
}

scrub_config_jsons() {
  local model="$1"
  local config_root
  config_root="$(dirname "$WT_MODELS_JSON")"
  [[ -d "$config_root" ]] || {
    warn "config root not present: $config_root"
    return 0
  }

  local vendor
  vendor="$(model_field "$model" vendor)"
  [[ -n "$vendor" ]] || vendor="$model"

  local file tmp changed
  shopt -s nullglob
  for file in "$config_root"/*.json; do
    [[ "$(realpath -m "$file")" == "$(realpath -m "$WT_MODELS_JSON")" ]] && continue
    jq empty "$file" 2>/dev/null || {
      warn "skipping non-JSON or invalid: $file"
      continue
    }
    tmp="$(mktemp)"
    jq --arg vendor "$vendor" --arg model "$model" '
      if (.preferred_vendors | type) == "array" then
        .preferred_vendors |= map(select(. != $vendor and . != $model))
      else . end
      | if (.blocked_models | type) == "array" then
          .blocked_models = ((.blocked_models // []) + [$model] | unique)
        else . end
      | if (.blocked_vendors | type) == "array" then
          .blocked_vendors = ((.blocked_vendors // []) + [$vendor] | unique)
        else . end
    ' "$file" >"$tmp"
    if ! diff -q "$file" "$tmp" >/dev/null 2>&1; then
      changed=1
      if [[ "$WT_DRY_RUN" == "1" ]]; then
        log "DRY-RUN: would rewrite $file (scrub vendor=$vendor model=$model)"
        rm -f "$tmp"
      else
        backup_file "$file"
        mv "$tmp" "$file"
        log "WROTE: $file (scrubbed vendor=$vendor model=$model)"
      fi
    else
      rm -f "$tmp"
    fi
  done
  shopt -u nullglob
  [[ -n "${changed:-}" ]] || log "no config JSON rewrites needed for $model"
}
