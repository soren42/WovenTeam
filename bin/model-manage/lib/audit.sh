#!/usr/bin/env bash

generate_audit_report() {
  local model="$1"
  local action="$2"
  local report="$WT_AUDIT_ROOT/model-manage-$action-$model-$WT_RUN_TS.md"
  local template
  template="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/templates/audit-report.md"

  local vendor workgroup registry_entry ownership_block
  vendor="$(model_field "$model" vendor 2>/dev/null || true)"
  workgroup="$(model_field "$model" workgroup 2>/dev/null || true)"
  registry_entry="$(jq --arg model "$model" '.models[$model] // {}' "$WT_MODELS_JSON" 2>/dev/null || echo '{}')"

  if [[ "$workgroup" == "coordination-api" ]]; then
    ownership_block=$'Coordination API ownership transferred per §6:\n\n- API spec: Claude (primary) + ChatGPT (secondary)\n- status.compile: Claude\n- Meeting / escalation: ChatGPT\n- Versioning & review: Gemini'
  else
    ownership_block="No coordination-api ownership change for this action."
  fi

  local rendered
  rendered="$(awk \
    -v ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    -v action="$action" \
    -v model="$model" \
    -v vendor="$vendor" \
    -v workgroup="$workgroup" \
    -v dry="$WT_DRY_RUN" \
    -v registry="$WT_MODELS_JSON" \
    -v logfile="$WT_LOG_FILE" \
    -v entry="$registry_entry" \
    -v ownership="$ownership_block" \
    -v failures="${WT_VERIFY_FAILURES:-0}" \
    '
    {
      gsub(/\{\{TIMESTAMP\}\}/, ts)
      gsub(/\{\{ACTION\}\}/, action)
      gsub(/\{\{MODEL\}\}/, model)
      gsub(/\{\{VENDOR\}\}/, vendor)
      gsub(/\{\{WORKGROUP\}\}/, workgroup)
      gsub(/\{\{DRY_RUN\}\}/, dry)
      gsub(/\{\{REGISTRY\}\}/, registry)
      gsub(/\{\{LOG\}\}/, logfile)
      gsub(/\{\{REGISTRY_ENTRY\}\}/, entry)
      gsub(/\{\{OWNERSHIP_BLOCK\}\}/, ownership)
      gsub(/\{\{VERIFY_FAILURES\}\}/, failures)
      print
    }' "$template")"

  printf '%s\n' "$rendered" | write_file_from_stdin "$report"
}
