# Model management

`bin/model-manage/wt-model-manage.sh` implements the guarded model-management
workflow defined in
`strategy-chat-transcripts/Solarian WovenTeam — Model Exfiltration & Rotation Script.md`.

## Registry

The live registry is:

```sh
/woventeam/config/models.json
```

It is the source of truth for active models, blocked vendors, and removal
metadata.

## Commands

```sh
bin/model-manage/wt-model-manage.sh --dry-run audit deepseek
bin/model-manage/wt-model-manage.sh --dry-run verify deepseek
bin/model-manage/wt-model-manage.sh --dry-run remove deepseek
bin/model-manage/wt-model-manage.sh rotate-keys
bin/model-manage/wt-model-manage.sh rotate-webhooks
```

## Safety behavior

- Destructive workflows require explicit `YES` operator confirmation with a
  read timeout (default 900s).
- `--dry-run` logs intended actions without mutating files or services.
- Logs go to `/woventeam/logs/model-manage-<timestamp>.log`.
- Backups are written under `/woventeam/backups/model-manage-<timestamp>/`.
- Audit reports default to `/woventeam/docs/audit/`.
- `verify` exits non-zero when any check fails, so it is safe to use as a
  CI gate.

## Implemented now

- Live model registry support and validation.
- `audit`, `verify`, `remove`, `rotate-keys`, `rotate-webhooks`, `add`
  command dispatch.
- Case-insensitive inventory scanning.
- Credential scrub for the target model's registered env var across
  `/etc/environment`, `~woven/.bashrc`, `~woven/.zshrc`, `~woven/.env`,
  `/woventeam/.env`.
- Redis key cleanup for `*<model>*` and `*ds_*` patterns followed by
  `BGSAVE`.
- SQLite schema-aware cleanup: marks matching `instances` rows
  `status='removed'` and `tasks` rows `status='reassign-needed'`; inserts
  an `audit_log` row if the table exists.
- Forward-looking config scrub: removes vendor from `preferred_vendors`
  arrays and adds to `blocked_models` / `blocked_vendors` arrays in any
  `/woventeam/config/*.json` other than `models.json`.
- Coordination-API ownership-transfer record written to
  `docs/audit/coordination-api-ownership-<date>.md` when removing a model
  whose registered workgroup is `coordination-api`.
- Audit reports rendered from `bin/model-manage/templates/audit-report.md`
  with vendor, workgroup, registry snapshot, ownership transfer, and
  verification-failure count.
- Operator blocking prompts for Slack channel archive/app removal and
  webhook rotation.
- Verification suite that hard-fails on residual registry state, missing
  blocked-vendor entry, Redis residuals, and residual env-var references.

## Intentionally still operator steps

- GitHub / Gitea deploy-key revocation.
- SSH key rotation for the `woven` user.
- Redis AUTH password rotation.
- Slack admin actions: archiving channels, removing apps, regenerating
  incoming webhook URLs.
- Strategy-doc / API-doc / dependency-graph rewrites are surfaced via
  `prepend_provenance_note` (in `lib/docs.sh`) rather than executed
  automatically — the spec requires preserving historical authorship for
  audit-trail integrity.
- Full onboarding automation for `add`.

These are deliberately gated behind operator action because they require
workspace-admin credentials, destructive remote API calls, or deliberate
narrative-doc edits the operator should review before applying.
