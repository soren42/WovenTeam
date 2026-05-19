# Phase 1 Operations Runbook

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
