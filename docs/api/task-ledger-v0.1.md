# Task Ledger v0.1

Phase 0 task packages use an append-only JSON Lines ledger:

```text
data/task-packages.jsonl
```

Each line is one complete JSON object. The initial record for a task should
validate against `docs/api/task-package-v0.1.json` and use:

```json
{
  "schema": "woventeam.task_package.v0.1",
  "taskId": "task_example_001",
  "initiativeId": "init_example",
  "createdBy": "ceo",
  "assignedRole": "backend_dev",
  "assignedAgent": "chatgpt",
  "modelId": "openai/gpt-5.3-codex",
  "priority": "normal",
  "status": "queued",
  "task": {
    "title": "Implement a small backend change",
    "body": "Make the requested code change and run the relevant test.",
    "deliverables": ["code change", "test result"]
  },
  "contextRefs": [],
  "acceptanceCriteria": ["The relevant test passes."],
  "toolPolicy": {
    "profile": "repo_branch",
    "filesystem": "workspace_write",
    "network": "none",
    "system": "none",
    "git": "branch_only"
  },
  "budget": {
    "timeoutSeconds": 1800,
    "maxOutputBytes": 1048576,
    "maxCostUsd": 1.0,
    "maxTokens": 2000000
  },
  "dependencies": [],
  "timestamps": {
    "createdAt": "2026-05-16T00:00:00Z",
    "updatedAt": "2026-05-16T00:00:00Z"
  }
}
```

Status changes should append a new event record rather than rewriting earlier
lines:

```json
{
  "schema": "woventeam.task_event.v0.1",
  "taskId": "task_example_001",
  "eventType": "status",
  "status": "running",
  "message": "Adapter runner accepted the task.",
  "createdBy": "wt-agent@chatgpt",
  "createdAt": "2026-05-16T00:01:00Z"
}
```

Manager-driven subtasks append an explicit request record before the generated
child package:

```json
{
  "schema": "woventeam.task_request.v0.1",
  "taskId": "task_child_001",
  "parentTaskId": "task_parent_001",
  "requestedTaskId": "task_child_001",
  "initiativeId": "init_example",
  "requestedBy": "project_manager",
  "requestedByRole": "project_manager",
  "requestedRole": "backend_dev",
  "assignedAgent": "chatgpt",
  "title": "Implement a small backend change",
  "body": "Make the requested code change.",
  "toolPolicy": {
    "profile": "observe"
  },
  "createdAtUnixMs": 1778917466000
}
```

`wt-roomd` validates the request against the Phase 0 spawn policy, appends the
request, emits a `task.request` room message, appends the generated child task
package, and emits `task.assign`. The child package includes `parentTaskId`,
`requestedByRole`, and a dependency on the parent task.

`wt-roomd` exposes the Phase 0 ledger through:

- `POST /api/task-package`
- `POST /api/task-request`
- `POST /api/task-event`
- `GET /api/tasks`
- `GET /api/task-summaries`
- `GET /api/task-detail?taskId=...`
- `GET /api/initiatives`
- `GET /api/initiative-detail?initiativeId=...`
- `GET /api/agents`
- `GET /api/task-artifacts?taskId=...`
- `GET /api/capacity`
- `GET /api/tokens`
- `GET /api/config`
- `POST /api/config`
- `POST /api/task-gate`
- `POST /api/task-usage`
- `POST /api/agent-control`
- `POST /api/task-reclaim`

`bin/wt-task` wraps those endpoints for local operator use with `create`,
`request`, `list`, `show`, `assign`, `update-status`, `retry`, `cancel`,
`close`, `reopen`, `reclaim`, `initiative create/list/show/export/close`, and
`agent list/pause/resume` commands.

Phase 1 keeps the JSONL ledger as the recovery source and adds a rebuildable
SQLite projection configured by `taskProjectionDbPath`. `wt-roomd` rebuilds the
projection at startup and before projected reads. `GET /api/task-summaries`
returns the current task rows from that projection, and
`GET /api/task-detail?taskId=...` returns one task plus its projected event
timeline. `GET /api/task-artifacts?taskId=...` exposes the adapter workspace
file list and previews of `result.md`, `stdout.log`, `stderr.log`, and
`manifest.json`. If the SQLite file is lost, it can be deleted and regenerated
from `data/task-packages.jsonl`.

Phase 2 derives initiatives from task package `initiativeId` values. `GET
/api/initiatives` returns initiative summaries with task counts, active task
counts, blocked/open-gate counts, token allocation, and timestamps. `GET
/api/initiative-detail?initiativeId=...` returns the same summary plus the
projected task rows for that initiative. There is no separate durable
initiative table yet; the task ledger remains authoritative.

`GET /api/tokens` reports token allocation from task package budgets and actual
reported usage from `woventeam.task_usage.v0.1` records. Allocation sums
`budget.maxTokens` over rolling 24-hour and 30-day windows and is enforced as a
hard stop before new task packages or manager subtasks are accepted.

Usage events use:

```json
{
  "schema": "woventeam.task_usage.v0.1",
  "taskId": "task_example_001",
  "provider": "openai",
  "modelId": "openai/gpt-5.3-codex",
  "inputTokens": 100,
  "outputTokens": 250,
  "totalTokens": 350,
  "estimatedCostCents": 7,
  "createdBy": "wt-agent@chatgpt",
  "createdAtUnixMs": 1778917466000
}
```

`GET /api/capacity` reports active task counts by agent, initiative, and parent
task. `wt-roomd` uses the same projection-backed counts to enforce
`maxActiveTasksPerAgent`, `maxSubtasksPerParent`, and `maxTasksPerInitiative`
when routing new task packages or manager subtasks.

`POST /api/task-gate` records operator review decisions as append-only task
events with `eventType=review_gate`. Supported actions are `approve`, `reject`,
and `revision`.

Phase 2 workload control adds task lease events and agent control records. Before
`wt-agent` starts work, it appends a normal task event with
`eventType=lease`, `status=leased`, `assignedAgent`, `attempt`, and
`leaseExpiresAtUnixMs`. The projection exposes `attemptCount`, `leaseOwner`,
`leasedAtUnixMs`, `runningAtUnixMs`, and `leaseExpiresAtUnixMs` on task summary
and detail rows.

Agent pause/resume records use:

```json
{
  "schema": "woventeam.agent_control.v0.1",
  "agent": "chatgpt",
  "action": "pause",
  "message": "Operator pause.",
  "createdBy": "ceo",
  "createdAtUnixMs": 1779205447000
}
```

`GET /api/agents` returns one row for each primary agent with active, leased,
running, stuck, and attempt counts. A task is considered stuck when it remains
`leased` or `running` for more than 15 minutes.

Sprint 3 closeout (2026-05-20) adds two more event shapes. A `reclaim` task
event releases the most recent lease and sends the task back to the queued
pool:

```json
{
  "schema": "woventeam.task_event.v0.1",
  "taskId": "task_example_001",
  "eventType": "reclaim",
  "status": "queued",
  "assignedAgent": "chatgpt",
  "reclaimReason": "lease_expired",
  "message": "Auto-reclaim after lease expired (previous holder=chatgpt).",
  "createdBy": "wt-agent@gemini",
  "createdAtUnixMs": 1779249000000
}
```

`reclaimReason` is an allowlist of `operator` (operator-triggered) or
`lease_expired` (recorded automatically by `wt-agent` when it finds a stuck
foreign lease). Failed status events may additionally carry a `retryCause`
field:

```json
{
  "schema": "woventeam.task_event.v0.1",
  "taskId": "task_example_001",
  "eventType": "status",
  "status": "failed",
  "message": "Codex adapter exitCode=124 timedOut=true; see task workspace manifest.",
  "retryCause": "timeout",
  "createdBy": "wt-agent@chatgpt",
  "createdAtUnixMs": 1779249000000
}
```

`retryCause` values emitted by `wt-agent` adapters: `timeout` (timeout kill),
`adapter_unavailable` (exec returned 127), `exit_nonzero` (any other non-zero
exit). The projection surfaces the latest classified cause on each task as
`failureCause`, the most recent reclaim's reason as `lastReclaimReason`, and a
cumulative `reclaimCount`. `POST /api/task-reclaim` accepts a JSON body with
`taskId`, `reason` (`operator` or `lease_expired`), optional `message`, and
optional `createdBy`; it appends the same `reclaim` task event a stuck-task
recovery would emit.

When a task is marked `blocked`, queued/running child tasks that list it in
`dependencies` receive an appended `blocked` task event. Terminal dependents are
not rewritten.

Codex adapter runs also write an artifact manifest under the per-task runtime
workspace. The manifest uses:

```json
{
  "schema": "woventeam.adapter_manifest.v0.1",
  "taskId": "task_example_001",
  "adapter": "codex",
  "command": "codex",
  "workspace": "/woventeam/runtime/tasks/task_example_001",
  "stdout": "/woventeam/runtime/tasks/task_example_001/stdout.log",
  "stderr": "/woventeam/runtime/tasks/task_example_001/stderr.log",
  "result": "/woventeam/runtime/tasks/task_example_001/result.md",
  "timedOut": false,
  "exitCode": 0
}
```

This keeps the Phase 0 storage model consistent with the room transcript:
append-only, inspectable, and easy to recover after process restarts. The
SQLite projection indexes this ledger without changing the package contract.
