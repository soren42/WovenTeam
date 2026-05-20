# Phase 1 / Phase 2 Operations Runbook

> Phase 2 (workload control, artifact promotion, policy + audit, launch
> rehearsal) extends the same operational model documented below for Phase 1.
> The Phase 2 additions are called out in the matching sub-sections.

## Health

Use the daemon health endpoint for the launch-era readiness check:

```sh
curl -fsS http://127.0.0.1:8787/api/health | jq .
```

The response reports daemon reachability, room name, ledger presence, room log
presence, projection rebuild status, and adapter enablement. A healthy runtime
has `ok: true` and `projection.ok: true`.

Service checks:

```sh
systemctl is-active wt-roomd.service wt-agent@claude.service wt-agent@chatgpt.service wt-agent@gemini.service
journalctl -u wt-roomd.service -n 80 --no-pager
journalctl -u 'wt-agent@*.service' -n 80 --no-pager
```

## Quotas

`GET /api/tokens` now separates allocation from actual usage:

- allocation: task package `budget.maxTokens`
- actual usage: appended `woventeam.task_usage.v0.1` records

`wt-roomd` enforces daily and monthly allocation hard stops before accepting a
new task package or manager subtask. The web composer displays projected budget
pressure before launch.

Adapters or operators can report usage:

```sh
curl -fsS -H 'Content-Type: application/json' \
  -d '{"taskId":"task_example","provider":"openai","modelId":"openai/gpt-5.3-codex","inputTokens":100,"outputTokens":250,"estimatedCostCents":7}' \
  http://127.0.0.1:8787/api/task-usage
```

## Backup

Backups are local filesystem copies of config, ledgers, projection, agent state,
and runtime artifacts. Preview first:

```sh
./bin/wt-ops-backup --dry-run
```

Create a backup:

```sh
./bin/wt-ops-backup
```

The default destination is `backups/woventeam-<UTC timestamp>/`, which is
ignored by git.

## Restore

Restore is deliberately gated. Preview:

```sh
./bin/wt-ops-restore --latest --dry-run
```

Restore only after stopping services or otherwise ensuring no writer is active:

```sh
sudo systemctl stop wt-agent@claude.service wt-agent@chatgpt.service wt-agent@gemini.service wt-roomd.service
./bin/wt-ops-restore --latest --yes
sudo systemctl start wt-roomd.service wt-agent@claude.service wt-agent@chatgpt.service wt-agent@gemini.service
```

Then verify `/api/health` and `/api/task-summaries`.

## End-to-End Validation

Run the final Phase 1 launch-era harness before a release or service restart:

```sh
make test-phase1-e2e
```

The harness starts an isolated daemon, routes a manager-created child task,
runs the opt-in Claude artifact adapter through a deterministic fake CLI,
checks artifact previews and review gates, records actual token usage, covers
failed/blocked/retry/closed lifecycle states, exercises backup and restore
previews, restarts `wt-roomd`, and confirms state rebuild from the JSONL ledger.

## Phase 2 Surfaces

Phase 2 adds workload control (Sprint 3), artifact promotion (Sprint 4),
policy + audit (Sprint 5), and the launch rehearsal harness (Sprint 6). Every
addition is additive and key-value config-gated.

### Policy

The central policy evaluator owns blocked-vendor, model-agent compatibility,
per-initiative + per-model-family budget, and capacity decisions for both
`POST /api/task-package` and `POST /api/task-request`. Knobs:

```ini
# Comma-separated vendor prefixes. Defaults to "deepseek" (trust-boundary
# decision from /woventeam/docs/audit/model-manage-remove-deepseek-...).
blockedVendors=deepseek,xai

# 0 disables the check. Otherwise the daemon refuses task packages whose
# initiative would exceed the cap when summing active maxTokens.
tokenBudgetPerInitiative=0

# Same idea grouped by modelId family (prefix before the slash).
tokenBudgetPerModelFamily=0
```

Every denial appends a durable `woventeam.policy_decision.v0.1` record with
the classified reason code (`vendor_blocked`, `model_agent_mismatch`,
`initiative_budget`, `model_family_budget`, `daily_budget`, `monthly_budget`,
`capacity_agent`, `capacity_initiative`).

### Artifact Promotion

Reviewed adapter output is promoted via `POST /api/task-artifact` (states:
`draft`, `reviewed`, `accepted`, `rejected`, `superseded`). CLI:

```sh
./bin/wt-task artifact promote task_example --path result.md --reviewer ceo --notes "ships"
./bin/wt-task artifact list init_example
./bin/wt-task artifact export task_example --out /tmp/result.md
```

The web console adds an artifact decision panel under the task detail and a
promoted-asset inventory under each initiative.

### Workload Control

`wt-agent` records lease events, auto-reclaims expired foreign leases, and
classifies adapter failures as `timeout`, `adapter_unavailable`, or
`exit_nonzero`. Operators have direct control too:

```sh
./bin/wt-task agent list
./bin/wt-task agent pause chatgpt --message "Codex maintenance"
./bin/wt-task agent resume chatgpt
./bin/wt-task reclaim task_example --reason operator --message "Manual unblock"
```

The console agent panel surfaces stuck-task counts and a `RECLAIM TASK`
button when an agent has at least one stuck task.

### Audit Export

`GET /api/initiative-audit?initiativeId=...` returns one structured document
with summary, tasks, events, policy decisions, and usage. CLI:

```sh
./bin/wt-task audit init_example --out /tmp/audit.json
jq '{tasks: (.tasks|length), events: (.events|length), policy: (.policyDecisions|length), usage: (.usage|length)}' /tmp/audit.json
```

The console adds an Audit button to each initiative card that opens the same
JSON in a new tab.

### Launch Rehearsal

`bin/wt-rehearse-live` runs one bounded real-CLI task end-to-end with an
operator-mandatory `--yes` flag. Default mode is dry-run; it prints adapter
readiness, current policy + budget knobs, and the configured rehearsal
parameters, then exits without posting. To execute:

```sh
./bin/wt-rehearse-live --yes --agent claude --max-tokens 4000 \
  --initiative init_rehearse_$(date -u +%Y%m%d)
```

The script writes a rehearsal log to
`docs/launch/phase2-rehearsal-<UTC date>.md` and an audit dump to a
sibling `.json` file.

### Phase 2 End-to-End Test

```sh
make test-phase2-e2e
```

Covers initiative routing, blocked-vendor denial, per-initiative budget
denial, real-adapter preflight, lease + auto-reclaim recovery, artifact
promotion (review + promote + audit), operator-triggered reclaim,
audit-export contents, CLI parity, and `wt-roomd` restart + projection
rebuild from JSONL.
