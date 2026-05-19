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
- `GET /api/task-artifacts?taskId=...`
- `GET /api/capacity`
- `GET /api/tokens`
- `GET /api/config`
- `POST /api/config`
- `POST /api/task-gate`
- `POST /api/task-usage`

`bin/wt-task` wraps those endpoints for local operator use with `create`,
`request`, `list`, `show`, `assign`, `update-status`, `retry`, `cancel`,
`close`, and `reopen` commands.

Phase 1 keeps the JSONL ledger as the recovery source and adds a rebuildable
SQLite projection configured by `taskProjectionDbPath`. `wt-roomd` rebuilds the
projection at startup and before projected reads. `GET /api/task-summaries`
returns the current task rows from that projection, and
`GET /api/task-detail?taskId=...` returns one task plus its projected event
timeline. `GET /api/task-artifacts?taskId=...` exposes the adapter workspace
file list and previews of `result.md`, `stdout.log`, `stderr.log`, and
`manifest.json`. If the SQLite file is lost, it can be deleted and regenerated
from `data/task-packages.jsonl`.

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
