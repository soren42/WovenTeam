# Phase 2 Spike 3 Workload Control - 2026-05-19

## Scope

Phase 2 Spike 3 adds basic workload control so agents can be paused, resumed,
leased, observed for stuck work, and counted by attempts without relying on
systemd for routine operator actions.

## Deliverables

- `wt-agent` appends a `leased` task event before running work.
- Task summaries and task detail rows include lease owner, lease timestamps,
  lease expiry, running timestamp, and attempt count.
- `POST /api/agent-control` appends durable pause/resume records.
- `GET /api/agents` returns agent state, active/leased/running/stuck task
  counts, and attempt totals.
- `bin/wt-task agent list/pause/resume` exposes the workflow from CLI.
- The web console agent panel shows pause/resume controls plus leased, running,
  stuck, and attempt counts.
- `make test-agent-workload-control` covers pause prevention, resume execution,
  lease event projection, attempt count projection, and stuck lease detection.

## Current Model

Leases are normal task events:

```json
{
  "schema": "woventeam.task_event.v0.1",
  "taskId": "task_example",
  "eventType": "lease",
  "status": "leased",
  "assignedAgent": "chatgpt",
  "attempt": 1,
  "leaseExpiresAtUnixMs": 1779206000000
}
```

Pause/resume is a separate append-only agent control record:

```json
{
  "schema": "woventeam.agent_control.v0.1",
  "agent": "chatgpt",
  "action": "pause"
}
```

`wt-agent` checks the latest control state before claiming work. The projection
classifies a leased or running task as stuck after 15 minutes.

## Validation

```sh
make test-agent-workload-control
node --check web/app.js
```

## Remaining Work

This spike records one attempt per claim path but does not yet implement
automatic lease expiration/reclaim. Later Phase 2 work should add retry policy,
lease renewal for long real-adapter runs, and operator-visible reclaim actions.
