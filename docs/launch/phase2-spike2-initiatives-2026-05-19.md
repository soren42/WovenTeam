# Phase 2 Spike 2 Initiatives - 2026-05-19

## Scope

Phase 2 Spike 2 promotes initiatives from a loose `initiativeId` convention into
a first-class projected workbench view. The durable source remains the
append-only task ledger; initiative summaries are derived from task package and
task event rows.

## Deliverables

- `GET /api/initiatives` returns initiative summaries.
- `GET /api/initiative-detail?initiativeId=...` returns one initiative summary
  plus projected task rows.
- `bin/wt-task initiative create/list/show/export/close` wraps the new operator
  workflow.
- The web console shows an initiative summary strip and can focus the task table
  by selected initiative.
- `make test-initiatives` covers API, CLI, task projection, and close behavior.

## Current Model

There is no separate durable initiative record yet. `initiative create` creates
the initiative's first manager/charter task with a chosen or generated
`initiativeId`. The SQLite projection groups tasks by that ID and calculates:

- total tasks;
- active tasks;
- complete, failed, and blocked tasks;
- open gate-style task count;
- allocated `maxTokens`;
- created and updated timestamps.

## Validation

```sh
make test-initiatives
node --check web/app.js
```

## Remaining Work

Later Phase 2 work should add accepted initiative artifacts, richer initiative
audit export formatting, and explicit initiative metadata if operators need
state that is not naturally represented by the charter task.
