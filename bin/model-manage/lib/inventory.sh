#!/usr/bin/env bash

inventory_model() {
  local model="$1"
  local inv_file="$WT_LOG_ROOT/inventory-$WT_RUN_TS.txt"
  mkdir -p "$WT_LOG_ROOT"
  log "inventory scan started for $model"
  log "inventory scan_root=$WT_SCAN_ROOT pattern=$model (case-insensitive); output=$inv_file"

  # Excluded dirs prevent a feedback loop: $WT_LOG_ROOT and $WT_BACKUP_ROOT
  # live inside the default scan root, so without excluding them grep would
  # read its own growing output. `audit` is excluded for the same reason
  # (the ownership transfer doc this tool writes contains the model name).
  grep -RIin \
    --exclude-dir=.git \
    --exclude-dir=logs \
    --exclude-dir=backups \
    --exclude-dir=audit \
    --exclude-dir=node_modules \
    -m 50 \
    -- "$model" "$WT_SCAN_ROOT" 2>/dev/null >"$inv_file" || true

  local hits
  hits="$(wc -l <"$inv_file" 2>/dev/null || echo 0)"
  log "inventory hits: $hits (full list: $inv_file)"
  if [[ "$hits" -gt 0 && "$hits" -le 50 ]]; then
    tee -a "$WT_LOG_FILE" <"$inv_file" >/dev/null
  elif [[ "$hits" -gt 50 ]]; then
    head -n 50 "$inv_file" | tee -a "$WT_LOG_FILE" >/dev/null
    log "inventory output truncated to first 50 lines in main log; see $inv_file"
  fi
}
