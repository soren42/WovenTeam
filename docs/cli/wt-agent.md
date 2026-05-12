# wt-agent

`wt-agent` is the Phase 0 C worker process. It preserves the existing Bash-stub contract while moving task execution into compiled code.

## Default Contract

- Subscribes to Redis channel `wt:tasks:new`.
- Processes only messages whose `payload.assigned_to` matches `--agent`.
- Publishes acknowledgements to `wt:tasks:ack`.
- Publishes task results to `wt:tasks:result`.
- Publishes lifecycle events to `wt:events:log`.
- Updates the SQLite `tasks.status` field at `WOVENTEAM_DB`, defaulting to `/woventeam/woventeam.db`.
- Posts task completion to Slack when `/woventeam/config/slack_webhook.txt` exists.

`payload.command` is executed by the shell with stderr folded into stdout. A zero exit code marks the task `complete`; a nonzero exit code marks it `error`. Captured output is limited to 1 MiB; the agent continues draining the child process after that limit and marks the result payload as `truncated`.

SQLite is mandatory for task execution. If the task row cannot be moved to `in_progress`, the command is not run. If the final status update fails, the agent exits with an error because SQLite is the durable Phase 0 source of truth.

Expected Phase 0 table:

```sql
CREATE TABLE tasks (
  id TEXT PRIMARY KEY,
  initiative TEXT,
  assigned_to TEXT,
  status TEXT NOT NULL,
  created_at INTEGER DEFAULT (strftime('%s','now'))
);
```

## Build

```sh
make build/wt-agent
```

Required libraries:

- `hiredis`
- `jansson`
- `sqlite3`

## Run

```sh
bin/wt-agent --agent claude
```

`bin/wt-agent` is a small launcher that asks `make` whether `build/wt-agent` is current, rebuilds when needed, and then executes it. The compiled binary is intentionally ignored by Git.

Useful options:

```sh
bin/wt-agent --agent tester --once
bin/wt-agent --agent tester --task-json /tmp/task.json --no-redis --no-slack
```

The `--task-json` mode exists for deterministic local validation and CI. Normal Phase 0 operation should use Redis subscription mode.

In Redis mode, `--once` exits after the first matching task is processed. Messages assigned to other agents are ignored and do not stop the worker.

## Environment

```sh
WOVENTEAM_DB=/woventeam/woventeam.db
WOVENTEAM_REDIS_HOST=127.0.0.1
WOVENTEAM_REDIS_PORT=6379
WOVENTEAM_TASK_CHANNEL=wt:tasks:new
WOVENTEAM_ACK_CHANNEL=wt:tasks:ack
WOVENTEAM_RESULT_CHANNEL=wt:tasks:result
WOVENTEAM_EVENT_CHANNEL=wt:events:log
WOVENTEAM_SLACK_WEBHOOK_FILE=/woventeam/config/slack_webhook.txt
```

Slack posting uses `curl` and is best-effort. Use `--no-slack` for local tests or CI runs where no webhook should be called.

## Validation

```sh
make test
```

The default integration test creates a temporary SQLite database, runs one task through `wt-agent` without Redis or Slack, and verifies the task transitions to `complete`. Live Redis validation should additionally start an agent in `--once` mode and publish a task to `wt:tasks:new`.
