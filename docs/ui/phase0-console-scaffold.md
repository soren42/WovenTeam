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
  - `GET /api/initiatives`
  - `GET /api/initiative-detail?initiativeId=...`
  - `GET /api/agents`
  - `GET /api/task-artifacts?taskId=...`
  - `GET /api/capacity`
  - `GET /api/tokens`
  - `GET /api/config`
  - `GET /api/adapters`
  - `POST /api/task-package`
  - `POST /api/task-request`
  - `POST /api/task-gate`
  - `POST /api/task-usage`
  - `POST /api/agent-control`
  - `POST /api/config`
- The task table reads the rebuildable SQLite task projection and shows task
  status, parent task IDs, requested-by role, event count, assigned role, and
  assigned agent.
- The initiative workbench summary reads `/api/initiatives` and lets operators
  focus the task table by initiative without changing the underlying task
  package contract.
- The agent status panel reads `/api/agents` and exposes pause/resume controls
  for Claude, ChatGPT/Codex, and Gemini, including leased, running, stuck, and
  attempt counts.
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
- The task detail panel includes a Sprint 4 artifact decision sub-panel:
  artifact path selector (sourced from the workspace file list), reviewer
  field, notes textarea, and Mark Reviewed / Promote / Reject / Supersede /
  Export buttons that hit `POST /api/task-artifact`. Each decision shows up in
  the task detail meta line as `artifact: <state> <path>`.
- When an initiative is focused, the Sprint 4 promoted-assets panel appears
  under the task table and lists accepted artifacts (and pending decisions)
  pulled from `GET /api/initiative-artifacts`.
- The alerts panel now feeds from the task summaries projection: failed,
  stuck, blocked, and revision-requested rows are listed with a click-to-task
  affordance. No new backend API was required.
- The chat panel's initiative and role filter dropdowns are wired. They filter
  the task table (and through it the alert feed) without changing the live
  transcript view.
- The help overlay (`?` key or top-bar button) lists keyboard shortcuts and the
  CLI ↔ UI parity table so operators can move between `wt-task` and the
  console without context switching.

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
- `GET /api/initiatives`
- `GET /api/initiative-detail?initiativeId=...`
- `GET /api/agents`
- `GET /api/task-artifacts?taskId=...`
- `GET /api/adapters`
- `GET /api/capacity`
- `POST /api/task-package`
- `POST /api/task-request`
- `POST /api/task-gate`
- `POST /api/task-usage`
- `POST /api/agent-control`
- `POST /api/task-reclaim`
- `POST /api/task-artifact`
- `GET /api/initiative-artifacts?initiativeId=...`

The next backend step is to expose richer initiative, gate history, usage, and
agent state so the console can enable the remaining disabled controls.
