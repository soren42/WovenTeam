#!/usr/bin/env bash

run_network_checks() {
  local model="$1"
  log "network check started for $model"
  grep -i "$model" /etc/hosts 2>/dev/null | tee -a "$WT_LOG_FILE" || true
  if command -v ufw >/dev/null 2>&1; then
    ufw status 2>/dev/null | grep -i "$model" | tee -a "$WT_LOG_FILE" || true
  fi
}
