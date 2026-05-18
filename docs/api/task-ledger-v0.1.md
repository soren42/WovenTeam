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
- `GET /api/tokens`
- `GET /api/config`
- `POST /api/config`

`bin/wt-task` wraps those endpoints for local operator use with `create`,
`request`, `list`, `show`, `assign`, and `update-status` commands.

`GET /api/tokens` reports token allocation from task package budgets in the
ledger. Phase 0 does not yet claim adapter-reported token usage; it sums
`budget.maxTokens` over rolling 24-hour and 30-day windows and compares that
allocation against the configured budgets. `POST /api/config` currently limits
web writes to token telemetry settings so the console can adjust budget display
without exposing broader runtime controls.

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
append-only, inspectable, and easy to recover after process restarts. A later
SQLite projection can index this ledger without changing the package contract.
