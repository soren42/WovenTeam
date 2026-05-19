# Phase 0 Console Scaffold

The Phase 0 browser UI is `WovenTeam Console (Fullscreen)` served by
`wt-roomd` from the static files in `web/`.

## Current Behavior

- `web/index.html` provides the all-in-one Mission Control layout scaffold.
- `web/style.css` contains the fullscreen desktop console styling.
- `web/app.js` connects the live room transcript to:
  - `GET /api/messages`
  - `GET /events`
  - `GET /api/tasks`
  - `GET /api/task-summaries`
  - `GET /api/task-detail?taskId=...`
  - `GET /api/task-artifacts?taskId=...`
  - `GET /api/capacity`
  - `GET /api/tokens`
  - `GET /api/config`
  - `GET /api/adapters`
  - `POST /api/task-package`
  - `POST /api/task-request`
  - `POST /api/task-gate`
  - `POST /api/task-usage`
  - `POST /api/config`
- The task table reads the rebuildable SQLite task projection and shows task
  status, parent task IDs, requested-by role, event count, assigned role, and
  assigned agent.
- Selecting a task row opens the Phase 1 task detail panel with task body,
  event timeline, retry/cancel/close lifecycle controls, and review gate
  approve/reject/revision controls.
- The task detail panel loads adapter artifacts from `/api/task-artifacts`,
  including workspace file metadata plus result, stdout, stderr, and manifest
  previews.
- The program initiation composer can create CEO task packages or
  manager-driven subtasks. Subtask mode requires a parent task ID and uses the
  daemon-enforced Program Manager / Project Manager spawn policy.
- The previous placeholder modal has been removed. Unimplemented surfaces are
  greyed out and disabled in place.
- The token sub-panel separates allocation telemetry from actual reported usage.
  Allocation sums package `budget.maxTokens` values over rolling 24-hour and
  30-day windows; actual usage comes from task usage events.
- The rail settings button opens a runtime configuration panel for token
  telemetry and opt-in adapter settings. Saving writes the active Phase 0
  key/value config file.
- The vendor panel reads live adapter capability state from `/api/adapters`,
  including enabled state, configured command, resolved command path, and
  launchability.
- The vendor panel now displays the Phase 2 adapter preflight state and reason,
  including the latest adapter failure class when a failed run has been recorded
  in the task ledger.
- The capacity readout uses `/api/capacity` to show active routing pressure and
  the settings panel exposes role routing and capacity caps.
- The composer shows projected budget pressure and the daemon rejects packages
  that would exceed configured daily or monthly allocation budgets.
- The Phase 1 console-facing contract is now covered by
  `make test-phase1-e2e`, including task detail, artifacts, review gates,
  tokens, capacity, and restart recovery.

## Temporary Placeholders

The following surfaces intentionally stay visible but disabled until their
backend tools exist:

- Agent topology
- Command palette search/actions

## Backend Contract Needed Next

The scaffold now binds to the task package and Phase 1 state paths:

- `docs/api/task-package-v0.1.json`
- `docs/api/task-request-v0.1.json`
- `docs/api/task-ledger-v0.1.md`
- `GET /api/tasks`
- `GET /api/task-summaries`
- `GET /api/task-detail?taskId=...`
- `GET /api/task-artifacts?taskId=...`
- `GET /api/adapters`
- `GET /api/capacity`
- `POST /api/task-package`
- `POST /api/task-request`
- `POST /api/task-gate`
- `POST /api/task-usage`

The next backend step is to expose richer initiative, gate history, usage, and
agent state so the console can enable the remaining disabled controls.
