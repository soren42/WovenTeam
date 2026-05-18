# Phase 0 Launch Readiness - 2026-05-18

Phase 0 is ready to move from spike work into launch hardening. The current
system runs as native GNU/Linux services, exposes the browser console on the
intranet listener, and records task state in append-only JSONL ledgers.

## Live Capabilities

- `wt-roomd.service` serves the shared room, web console, SSE stream, task
  ledger APIs, token allocation APIs, and token configuration APIs.
- `wt-agent@claude.service`, `wt-agent@chatgpt.service`, and
  `wt-agent@gemini.service` run under systemd.
- The web console can create CEO task packages and manager subtask requests.
- The task ledger records packages, requests, status events, dependency
  blocking, and Codex adapter manifests.
- The token panel reports `budget.maxTokens` allocation over rolling 24-hour
  and 30-day windows.
- The settings rail can update token telemetry budgets in the active Phase 0
  key/value config file.

## Validation

The repository test suite passed with:

```sh
make test
```

The live service path was also verified against `http://127.0.0.1:8787`:

```sh
WT_ROOM_URL=http://127.0.0.1:8787 ./bin/wt-task create \
  --title "Phase 0 launch e2e token config" \
  --body "Live launch smoke: create a task, let the chatgpt agent claim it, and record the result in the room." \
  --role backend_dev \
  --agent chatgpt \
  --initiative init_launch_e2e \
  --max-tokens 500000
```

Observed result:

- The daemon accepted the task package.
- `wt-agent@chatgpt.service` appended a `running` task event.
- `wt-agent@chatgpt.service` appended a `complete` task event.
- The room transcript emitted `task.assign`, `task.status`, and `task.result`
  messages for the task.

## Launch Boundary

These items are intentionally not launch blockers for Phase 0:

- Review gate approve/reject controls.
- Adapter-reported actual token usage.
- Agent topology view.
- Full runtime settings beyond token telemetry.
- Capacity counters for role spawn limits.
- Work-capable Claude and Gemini adapters equivalent to the current opt-in
  Codex path.

Those belong in the launched system backlog rather than the Phase 0 spike
closure checklist.
