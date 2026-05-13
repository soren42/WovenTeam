#!/usr/bin/env bash

transfer_coordination_ownership() {
  local model="$1"
  local workgroup
  workgroup="$(model_field "$model" workgroup)"

  if [[ "$workgroup" != "coordination-api" ]]; then
    log "no special ownership transfer rules registered for $model (workgroup=$workgroup)"
    return 0
  fi

  log "coordination API ownership transfer rule matched for $model"
  local record="$WT_AUDIT_ROOT/coordination-api-ownership-$(date -u +%Y-%m-%d).md"
  write_file_from_stdin "$record" <<EOF
# Coordination API Ownership Transfer

- Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)
- Removed owner: $model
- Source rule: Solarian WovenTeam — Model Exfiltration & Rotation Script §6

## New ownership

| Component                              | New Owner                                  |
|----------------------------------------|--------------------------------------------|
| API Specification (schema definitions) | Claude (primary) + ChatGPT (secondary)     |
| Status Compilation (status.compile)    | Claude                                     |
| Meeting Facilitation protocols         | ChatGPT                                    |
| Escalation routing                     | ChatGPT                                    |
| API versioning & changelog             | Gemini (review authority)                  |

## Provenance

The Coordination API v0.9 specification is preserved; only attribution and
forward-looking ownership change. Historical authorship by the removed model
is intentionally retained in source documents for audit-trail integrity.
EOF
}
