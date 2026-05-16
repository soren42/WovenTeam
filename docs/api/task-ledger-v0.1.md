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
    "maxCostUsd": 1.0
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

This keeps the Phase 0 storage model consistent with the room transcript:
append-only, inspectable, and easy to recover after process restarts. A later
SQLite projection can index this ledger without changing the package contract.
