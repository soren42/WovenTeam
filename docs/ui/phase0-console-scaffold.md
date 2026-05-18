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
  - `GET /api/tokens`
  - `GET /api/config`
  - `POST /api/task-package`
  - `POST /api/task-request`
  - `POST /api/config`
- The task table reads the append-only task ledger and shows task status,
  parent task IDs, requested-by role, dependencies, assigned role, and assigned
  agent.
- The program initiation composer can create CEO task packages or
  manager-driven subtasks. Subtask mode requires a parent task ID and uses the
  daemon-enforced Program Manager / Project Manager spawn policy.
- The previous placeholder modal has been removed. Unimplemented surfaces are
  greyed out and disabled in place.
- The token sub-panel is live allocation telemetry. It sums package
  `budget.maxTokens` values over rolling 24-hour and 30-day windows, displays
  estimated cost from config, and does not yet claim adapter-reported usage.
- The rail settings button opens a runtime configuration panel for token
  telemetry. Saving writes the active Phase 0 key/value config file.

## Temporary Placeholders

The following surfaces intentionally stay visible but disabled until their
backend tools exist:

- Review gate approve/reject state
- Agent topology
- Vendor runtime settings
- Command palette search/actions

## Backend Contract Needed Next

The scaffold now binds to the Sprint 2-4 task package paths:

- `docs/api/task-package-v0.1.json`
- `docs/api/task-request-v0.1.json`
- `docs/api/task-ledger-v0.1.md`
- `GET /api/tasks`
- `POST /api/task-package`
- `POST /api/task-request`

The next backend step is to expose richer initiative, gate, adapter usage, and
agent state so the console can enable the remaining disabled controls.
