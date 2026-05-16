# Manager-Driven Subtasks

Sprint 4 adds the first manager-driven subtask path to the Phase 0 task
ledger. It keeps orchestration explicit: managers request subtasks through
`task.request`; the daemon validates role authority; the daemon then writes the
request, creates a queued task package, and emits room-visible events.

## API

`POST /api/task-request` accepts `woventeam.task_request.v0.1` records:

```json
{
  "schema": "woventeam.task_request.v0.1",
  "taskId": "task_child",
  "parentTaskId": "task_parent",
  "requestedTaskId": "task_child",
  "initiativeId": "init_example",
  "requestedBy": "project_manager",
  "requestedByRole": "project_manager",
  "requestedRole": "backend_dev",
  "assignedAgent": "chatgpt",
  "title": "Implement a small backend change",
  "body": "Make the requested change and report the result.",
  "toolPolicy": {
    "profile": "observe"
  }
}
```

Successful requests append two ledger records:

- The original `woventeam.task_request.v0.1` request.
- A generated `woventeam.task_package.v0.1` child task with
  `parentTaskId`, `requestedByRole`, and a `dependencies` entry for the parent.

The room log receives `task.request` followed by `task.assign`, so managers and
operators can see both the request and the concrete assignment.

## Spawn Policy

The Phase 0 daemon enforces the role authority currently documented in the
ROLE files:

| Requesting role | May request |
| --- | --- |
| `program_manager` | `project_manager`, `software_architect`, `systems_architect` |
| `project_manager` | Non-leadership architecture, implementation, quality, operations, and delivery roles within the initiative |

Requests outside that policy return `403` and are not appended to the ledger.
Concurrency caps remain documented in the ROLE files; Sprint 4 records the
policy boundary and validates role type, but does not yet maintain live
per-role capacity counters.

## Dependencies

Every task created from `task.request` depends on its parent task. When an
operator or agent marks a task `blocked`, `wt-roomd` appends `blocked` status
events for queued/running dependents that list the blocked task in
`dependencies`.

Terminal states are not rewritten. Completed, failed, cancelled, or already
blocked dependents are left as-is.

## CLI

Use `wt-task request` to create a manager-driven subtask:

```sh
./bin/wt-task request \
  --parent task_parent \
  --by-role project_manager \
  --role backend_dev \
  --agent chatgpt \
  --initiative init_example \
  --title "Implement fixture" \
  --body "Produce the worker artifact."
```

## Validation

`tests/integration/wt-manager-subtasks.sh` verifies:

- CEO creates a PM task.
- PM requests worker and tester subtasks.
- Policy denies a Program Manager request for direct implementation work.
- Worker and reviewer tasks can complete.
- Blocking a parent task propagates `blocked` to a dependent child task.
- `task.request`, `task.assign`, and `task.result` room messages are emitted.
