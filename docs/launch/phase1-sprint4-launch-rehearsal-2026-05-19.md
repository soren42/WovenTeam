# Phase 1 Sprint 4 Launch Rehearsal - 2026-05-19

## Scope

This rehearsal covers the Sprint 4 launch operations path: quota hard stops,
actual usage reporting, health checks, and backup/restore previews.

## Commands

```sh
make clean && make
node --check web/app.js
make test-quotas-ops
```

## Evidence

- Budget enforcement rejects a task package that would exceed the configured
  24-hour allocation budget.
- `POST /api/task-usage` appends an actual usage event.
- `GET /api/tokens` reports allocation and actual usage separately.
- `GET /api/health` reports ledger, room log, and projection health.
- `wt-ops-backup --dry-run` lists included runtime assets.
- `wt-ops-backup` writes config, ledger, state, projection, and runtime artifact
  copies under an ignored backup root.
- `wt-ops-restore --latest --dry-run` reports the restore plan without writing.

## Result

Sprint 4 operations checks passed in the integration environment. Live
deployment should still run a real backup before any restore and should stop
writers before a destructive restore.
