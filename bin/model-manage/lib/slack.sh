#!/usr/bin/env bash

# Slack admin operations referenced by §3.3 and §4.2 of the spec.
# Implemented as blocking operator prompts because automated Slack admin
# actions (archiving channels, removing apps, rotating webhooks) require
# workspace-admin credentials this script intentionally does not hold.

slack_cleanup_for_model() {
  local model="$1"
  local channels
  channels="$(jq -r --arg model "$model" '.models[$model].slack_channels // [] | .[]' "$WT_MODELS_JSON" 2>/dev/null)"
  if [[ -z "$channels" ]]; then
    log "no slack channels registered for $model; slack cleanup skipped"
    return 0
  fi
  log "slack channels owned by ${model}: ${channels//$'\n'/ }"
  blocking_prompt "MANUAL ACTION REQUIRED: in Slack admin, archive or rename the following channel(s) for $model: $channels. Then remove any Slack apps or incoming webhooks created for $model. (§3.3)"
}

slack_rotate_webhooks() {
  blocking_prompt "MANUAL ACTION REQUIRED: regenerate all Slack incoming webhook URLs and update /woventeam/config/slack_webhook.txt + any channel-specific webhook config. (§4.2)"
}
