# Phase 0 Console Scaffold

The Phase 0 browser UI is `WovenTeam Console (Fullscreen)` served by
`wt-roomd` from the static files in `web/`.

## Current Behavior

- `web/index.html` provides the all-in-one Mission Control layout scaffold.
- `web/style.css` contains the fullscreen desktop console styling.
- `web/app.js` connects the live room transcript to:
  - `GET /api/messages`
  - `GET /events`
  - `POST /api/message`
- The program initiation composer currently posts a structured CEO directive
  into the shared room. It does not create durable task packages yet.
- The modal close control is implemented as a 44 by 44 pixel touch target with
  click, pointer, touch, and delegated fallbacks for Safari on iPad.

## Temporary Placeholders

The following surfaces intentionally show coming-soon placeholders until their
backend tools exist:

- Initiative/task package table
- Task package create/list/update endpoints
- Review gate approve/reject state
- Token and cost telemetry
- Agent topology
- Vendor runtime settings
- Command palette search/actions

## Backend Contract Needed Next

The scaffold is ready to bind to the Sprint 1 runtime contracts:

- `docs/api/task-package-v0.1.json`
- `docs/api/task-ledger-v0.1.md`

The next backend step is to add task package APIs and a task ledger projection
so the console can replace placeholders with live initiative, gate, token, and
agent state.
