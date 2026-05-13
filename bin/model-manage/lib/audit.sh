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

  # Bash 5.2 enables `patsub_replacement` by default, which makes `&` in the
  # replacement string of ${var//pat/repl} mean "the matched pattern" —
  # corrupting any payload that contains a literal `&` (e.g. "Versioning &
  # review"). Disable it so the replacement is literal.
  local _prev_patsub
  _prev_patsub="$(shopt -p patsub_replacement 2>/dev/null || true)"
  shopt -u patsub_replacement 2>/dev/null || true

  local rendered
  rendered="$(<"$template")"
  rendered="${rendered//\{\{TIMESTAMP\}\}/$(date -u +%Y-%m-%dT%H:%M:%SZ)}"
  rendered="${rendered//\{\{ACTION\}\}/$action}"
  rendered="${rendered//\{\{MODEL\}\}/$model}"
  rendered="${rendered//\{\{VENDOR\}\}/$vendor}"
  rendered="${rendered//\{\{WORKGROUP\}\}/$workgroup}"
  rendered="${rendered//\{\{DRY_RUN\}\}/$WT_DRY_RUN}"
  rendered="${rendered//\{\{REGISTRY\}\}/$WT_MODELS_JSON}"
  rendered="${rendered//\{\{LOG\}\}/$WT_LOG_FILE}"
  rendered="${rendered//\{\{REGISTRY_ENTRY\}\}/$registry_entry}"
  rendered="${rendered//\{\{OWNERSHIP_BLOCK\}\}/$ownership_block}"
  rendered="${rendered//\{\{VERIFY_FAILURES\}\}/${WT_VERIFY_FAILURES:-0}}"

  printf '%s\n' "$rendered" | write_file_from_stdin "$report"

  eval "$_prev_patsub" 2>/dev/null || true
}
