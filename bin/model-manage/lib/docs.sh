#!/usr/bin/env bash

# Documentation hygiene referenced by §3.8 and §3.9 of the spec.
# Forward-looking docs (team rosters, dependency graphs, ownership maps) should
# be updated when a model is removed; historical docs that the removed model
# authored should receive a provenance header but their bodies must not be
# rewritten so the audit trail stays intact.

PROVENANCE_TEMPLATE_PATH="${PROVENANCE_TEMPLATE_PATH:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/templates/provenance-note.md}"

prepend_provenance_note() {
  local doc="$1"
  local model="$2"
  [[ -f "$doc" ]] || {
    warn "doc not found for provenance prepend: $doc"
    return 0
  }
  if grep -q "PROVENANCE NOTE" "$doc"; then
    log "provenance note already present: $doc"
    return 0
  fi
  if [[ "$WT_DRY_RUN" == "1" ]]; then
    log "DRY-RUN: would prepend provenance note (model=$model) to $doc"
    return 0
  fi
  backup_file "$doc"
  local tmp
  tmp="$(mktemp)"
  {
    sed "s/{{REMOVED_MODEL}}/$model/g; s/{{REMOVED_DATE}}/$(date -u +%Y-%m-%d)/g" "$PROVENANCE_TEMPLATE_PATH"
    printf '\n'
    cat "$doc"
  } >"$tmp"
  mv "$tmp" "$doc"
  log "prepended provenance note to $doc"
}
