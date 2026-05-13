#!/usr/bin/env bash

inventory_model() {
  local model="$1"
  log "inventory scan started for $model"
  log "inventory scan_root=$WT_SCAN_ROOT pattern=$model (case-insensitive)"
  grep -RIin --exclude-dir=.git -- "$model" "$WT_SCAN_ROOT" 2>/dev/null | tee -a "$WT_LOG_FILE" || true
}
