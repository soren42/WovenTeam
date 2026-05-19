# Phase 2 Spike 1 Adapter Preflight - 2026-05-19

## Scope

Phase 2 Spike 1 begins real adapter hardening by separating command
launchability from operational readiness. The existing `/api/adapters` endpoint
keeps its top-level compatibility fields and now adds a `preflight` object for
each primary adapter.

## Deliverables

- `GET /api/adapters` reports strict preflight readiness for Codex, Claude, and
  Gemini adapters.
- `bin/wt-adapter-preflight` exposes the same report for CLI operators.
- The web console vendor panel shows preflight state, readiness reason, and the
  latest adapter failure class.
- Adapter docs describe the preflight contract and failure classes.
- Integration coverage verifies ready, disabled, invalid mode, missing command,
  and ledger-derived latest failure paths.

## Readiness Checks

Preflight verifies:

- adapter is enabled;
- Claude and Gemini are in `adapter` mode;
- command resolves to an executable;
- runtime root or its parent is writable;
- adapter timeout is greater than zero;
- output cap is greater than zero.

The top-level `state` remains command launchability for Phase 1 compatibility.
Use `preflight.ok` for launch decisions.

## Validation

```sh
make test-adapter-capabilities test-adapter-preflight
node --check web/app.js
```

## Remaining Work

The preflight surface does not yet perform real vendor auth/session probes. The
next hardening step is to add safe, provider-specific smoke probes for installed
Claude, Codex, and Gemini CLI sessions before enabling real work tasks.
