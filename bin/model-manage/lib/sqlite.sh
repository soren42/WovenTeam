#!/usr/bin/env bash

sqlite_table_exists() {
  local table="$1"
  [[ -f "$WT_DB_PATH" ]] || return 1
  sqlite3 "$WT_DB_PATH" "SELECT 1 FROM sqlite_master WHERE type='table' AND name='$table';" | grep -q 1
}

sqlite_column_exists() {
  local table="$1"
  local column="$2"
  [[ -f "$WT_DB_PATH" ]] || return 1
  sqlite3 "$WT_DB_PATH" "PRAGMA table_info($table);" | awk -F'|' '{print $2}' | grep -qx "$column"
}

sqlite_exec() {
  local sql="$1"
  if [[ "$WT_DRY_RUN" == "1" ]]; then
    log "DRY-RUN sqlite: $sql"
  else
    sqlite3 "$WT_DB_PATH" "$sql"
    log "RUN sqlite: $sql"
  fi
}

cleanup_model_sqlite() {
  local model="$1"
  if [[ ! -f "$WT_DB_PATH" ]]; then
    warn "database not found: $WT_DB_PATH"
    return 0
  fi

  local quoted_model
  quoted_model="$(printf "%s" "$model" | sed "s/'/''/g")"

  if sqlite_table_exists instances && sqlite_column_exists instances agent && sqlite_column_exists instances status; then
    local n
    n="$(sqlite3 "$WT_DB_PATH" "SELECT COUNT(*) FROM instances WHERE (agent LIKE '%${quoted_model}%' OR agent LIKE '%ds_%') AND status != 'removed';" 2>/dev/null || echo 0)"
    log "sqlite instances rows matching ${model}/ds_* (non-removed): $n"
    if [[ "$n" -gt 0 ]]; then
      sqlite_exec "UPDATE instances SET status = 'removed' WHERE (agent LIKE '%${quoted_model}%' OR agent LIKE '%ds_%') AND status != 'removed';"
    fi
  else
    warn "instances table or expected columns not present; skipping instance update"
  fi

  if sqlite_table_exists tasks && sqlite_column_exists tasks assigned_to && sqlite_column_exists tasks status; then
    local n
    n="$(sqlite3 "$WT_DB_PATH" "SELECT COUNT(*) FROM tasks WHERE (assigned_to LIKE '%${quoted_model}%' OR assigned_to LIKE '%ds_%') AND status NOT IN ('complete','removed','reassign-needed');" 2>/dev/null || echo 0)"
    log "sqlite tasks rows matching ${model}/ds_* (live, not complete): $n"
    if [[ "$n" -gt 0 ]]; then
      sqlite_exec "UPDATE tasks SET status = 'reassign-needed' WHERE (assigned_to LIKE '%${quoted_model}%' OR assigned_to LIKE '%ds_%') AND status NOT IN ('complete','removed','reassign-needed');"
    fi
  else
    warn "tasks table or expected columns not present; skipping task update"
  fi

  if sqlite_table_exists audit_log; then
    sqlite_exec "INSERT INTO audit_log (event_type, details, timestamp) VALUES ('model.removed', '{\"model\":\"${quoted_model}\",\"removed_by\":\"wt-model-manage\"}', datetime('now'));"
  else
    log "audit_log table not present; removal audit recorded only in /woventeam/logs and /woventeam/docs/audit"
  fi
}
