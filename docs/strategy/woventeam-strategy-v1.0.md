# WovenTeam Strategy v1.0

**Status:** active
**Last updated:** 2026-05-13
**Registry source of truth:** `/woventeam/config/models.json`
**Removal audit:** `/woventeam/docs/audit/`

## Model roster

| Model    | Vendor    | Tier | Workgroup           | Status  |
|----------|-----------|-----:|---------------------|---------|
| Claude   | Anthropic | 1    | dashboard           | active  |
| ChatGPT  | OpenAI    | 1    | cli-backend         | active  |
| Gemini   | Google    | 1    | review-integration  | active  |
| Qwen     | Alibaba   | 2    | documentation       | active  |
| DeepSeek | DeepSeek  | 3    | (removed)           | removed |

Tier definitions:

1. Full orchestration access — can touch state, infrastructure, and coordination bus.
2. Sandboxed execution — no persistent state propagation without approval.
3. Removed or untrusted — no access, blocked at all layers.

## Component ownership map

The Coordination API was originally owned by DeepSeek. After the 2026-05-12
removal, ownership transferred per §6 of the Model Exfiltration spec:

| Component                                                    | Owner                                  |
|--------------------------------------------------------------|----------------------------------------|
| Orchestration Engine (initiative mgr, task queue, scheduler) | ChatGPT                                |
| Coordination Layer — API specification                       | Claude (primary) + ChatGPT (secondary) |
| Coordination Layer — `status.compile`                        | Claude                                 |
| Coordination Layer — meeting facilitation                    | ChatGPT                                |
| Coordination Layer — escalation routing                      | ChatGPT                                |
| Coordination Layer — versioning & review                     | Gemini                                 |
| Autonomy & Trust (permissions, gates, rollback)              | ChatGPT                                |
| CLI Adapter contracts                                        | Claude                                 |
| Dashboard (web)                                              | Claude                                 |
| CLI / Backend framework (`wt-agent`, `wtctl`)                | ChatGPT                                |
| Documentation                                                | Qwen                                   |
| Review & integration testing                                 | Gemini                                 |

## Layer model

```
1. AGENT LAYER         — vendor model instances (Claude, ChatGPT, Gemini, Qwen)
2. ADAPTER LAYER       — vendor CLIs / wrappers; CLI Adapter contracts owned by Claude
3. RUNTIME LAYER       — wt-agent worker, wtctl dispatch, SQLite state, Redis pub/sub
4. COORDINATION LAYER  — Coordination API v0.9 (Claude + ChatGPT, reviewed by Gemini)
5. SURFACE LAYER       — web dashboard, audit reports, operator CLIs
```

The Coordination API v0.9 schemas (task assignment, status reporting, meeting
protocols, escalation, audit envelope) are preserved as originally drafted;
forward-looking ownership has transferred but the schemas themselves are sound
and remain authoritative.

## Operating rules adopted with the removal

- DeepSeek is **removed and blocked** at all layers. Its vendor is in
  `blocked_vendors` in the live registry; future agent IDs matching
  `*ds_*` or `*deepseek*` are rejected by the runtime.
- The `#coordination-api` Slack channel listed in the original plan was
  never actually realized in Slack; `models.json` records this with
  `slack_channel_status: "never_created"`. Future coordination-API
  discussion happens in the existing review/integration channels.
- Coordination-API ownership changes require review approval from Gemini
  (versioning & review authority).

## Tooling

- Model lifecycle is managed via `bin/model-manage/wt-model-manage.sh`.
  Operator documentation: `docs/cli/model-management.md`. Run with
  `--dry-run` for preflight.
- Worker: `src/worker/wt_agent.c` (built via `Makefile` → `bin/wt-agent`).
- Operator CLI: `bin/wtctl`.

## References

- Removal spec: `/woventeam/strategy-chat-transcripts/Solarian WovenTeam — Model Exfiltration & Rotation Script.md`
- Strategy chat history (annotated with provenance notes as of 2026-05-13):
  `/woventeam/strategy-chat-transcripts/`
- Removal audit artifacts: `/woventeam/docs/audit/`
- Live model registry: `/woventeam/config/models.json`
