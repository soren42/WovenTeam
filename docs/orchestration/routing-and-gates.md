# Routing And Gates

Phase 1 Sprint 3 adds a small managed-collaboration layer on top of the
append-only task ledger.

## Routing

When `roleRoutingEnabled=1`, `wt-roomd` treats an empty, `all`, or `router`
assignment as a routing request. The daemon chooses an agent from:

- requested role
- tool profile
- model family
- current active task capacity

Repository-writing profiles (`repo_branch`, `test_local`) route to `chatgpt`
because the Codex adapter is the bounded workspace path. Read-only planning
profiles (`observe`, `ops_read`) route by role: management and writing work to
`claude`, architecture and performance work to `gemini`, and implementation or
test work to `chatgpt`.

The current capacity caps are:

```text
maxActiveTasksPerAgent=4
maxSubtasksPerParent=8
maxTasksPerInitiative=32
```

The web settings panel can edit those values in the active runtime config.

## Capacity API

`GET /api/capacity` rebuilds the SQLite projection and returns active task
counts by agent, initiative, and parent task, plus the configured caps. Terminal
states are not counted as active.

## Review Gates

`POST /api/task-gate` appends a normal task event with
`eventType=review_gate`. Supported actions are:

- `approve` -> `approved`
- `reject` -> `rejected`
- `revision` -> `revision_requested`

Gate events are visible in task detail and the room transcript. They do not yet
create automatic follow-up tasks; that remains future revision workflow work.
