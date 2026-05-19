# Phase 1 Spike 6 End-to-End Validation - 2026-05-19

## Scope

Spike 6 closes Phase 1 by adding a single integration harness that exercises the
launch-era collaborative path from operator task creation through agent
execution, artifact inspection, review gates, usage telemetry, lifecycle state,
backup preview, and daemon restart recovery.

## Command

```sh
make test-phase1-e2e
```

`make test` also includes this target.

## Coverage

- Starts an isolated `wt-roomd` with temporary room, ledger, projection,
  runtime, config, and token settings.
- Verifies `/api/health` and `/api/adapters` before work starts.
- Creates an initiative manager task through `bin/wt-task create` and confirms
  role routing assigns `project_manager` work to Claude.
- Creates a manager-driven child task through `bin/wt-task request` and confirms
  the child task is linked to the parent and routed to Claude.
- Runs `wt-agent --agent claude --once` with the opt-in Claude artifact adapter
  pointed at a deterministic fake CLI.
- Confirms the child task records `running` and `complete` events.
- Confirms `/api/task-artifacts` exposes `prompt.md`, `result.md`,
  `stdout.log`, `stderr.log`, and `manifest.json` previews from the per-task
  workspace.
- Reports actual token usage through `POST /api/task-usage` and confirms
  `/api/tokens` separates allocated and actual usage.
- Exercises all review gate actions: `approve`, `reject`, and `revision`.
- Records representative `failed`, `blocked`, retry/queued, and `closed`
  lifecycle states.
- Verifies `/api/capacity` reflects configured routing caps.
- Exercises `wt-ops-backup --dry-run`, `wt-ops-backup`, and
  `wt-ops-restore --latest --dry-run`.
- Restarts `wt-roomd` after deleting the SQLite projection and confirms the
  task detail, artifacts, and token telemetry recover from the JSONL ledger and
  runtime workspace.

## Result

The isolated Phase 1 end-to-end harness passes. Phase 1 now has both targeted
slice tests and a whole-path test that proves the current system can accept
work, route it, run a controlled adapter, surface results, record operator
decisions, preserve telemetry, and recover state after daemon restart.

## Remaining Launch Risk

This test uses a deterministic fake Claude CLI so it can run safely in CI and
on developer machines. A production launch rehearsal should still run the same
path against the installed real CLI clients with a harmless read-only task and
operator-approved token budgets.
