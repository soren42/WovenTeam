# Phase 3 Sprint 2 Autonomy And Policy Contract - 2026-05-20

## Scope

Sprint 2 adds the v1.0 autonomy contract on top of Sprint 1's runtime
observability and cancel substrate. The implementation is additive so it can be
rebased behind Sprint 1: task packages can carry bounded autonomy grants, the
policy evaluator classifies grant failures, adapters receive elevated CLI flags
only when a grant is valid, and operators can revoke autonomy through CLI or web.

## Delivered

- `autonomyLevel` and `autonomyGrant` are accepted on task packages and manager
  requests. CLI flags: `--autonomy-level`, `--autonomy-ttl`,
  `--autonomy-scope`, `--autonomy-network`, `--credential-class`, and
  `--requires-clean-worktree`.
- Config defaults exist for global and per-agent autonomy:
  `defaultAutonomyLevel`, `claudeDefaultAutonomyLevel`,
  `chatgptDefaultAutonomyLevel`, `geminiDefaultAutonomyLevel`.
- The policy evaluator adds stable denial reasons:
  `autonomy_required`, `autonomy_expired`, `autonomy_revoked`.
- `wt-agent` records `woventeam.autonomy_event.v0.1` for autonomous claim paths,
  adapter invocations, elevated adapter invocations, and adapter exits.
- Codex adapter elevation maps to `--sandbox danger-full-access` plus
  `--ask-for-approval never`; non-elevated runs use `workspace-write` and
  `on-request`. Claude and Gemini adapter paths add their equivalent elevation
  flags only when a grant is active.
- `POST /api/autonomy-revoke` appends an autonomy revocation event and a
  `woventeam.kill_event.v0.1` with `reason:"autonomy-revoke"`. The next adapter
  invocation downgrades after revocation.
- Web UI parity: autonomy defaults in settings, composer grant controls, task
  detail autonomy meta, and a Revoke Autonomy action.
- `make test-autonomy` covers elevated Codex flags, autonomy event chain,
  missing/expired grant denials, revocation, kill-event emission, and downgrade.

## Review Notes

- Revocation uses Sprint 1's kill-event substrate. If Sprint 1 changes
  `wt_agent.c` wait-loop or cancel semantics, rebase this work behind that branch
  and re-run `make test-autonomy test-agent-workload-control test-codex-adapter`.
- The grant scope check is deliberately small for v1.0: `workspace`, `adapter`,
  or `*` authorizes elevated adapter invocation. It does not claim per-syscall or
  per-file observability after a vendor CLI receives skip-permission flags.
- `requiresCleanWorktree` is enforced by the agent before elevation. It is not a
  daemon-side admission check because the daemon may later run on a different
  host than the worker.

## Remaining Work

- Add autonomy events to the combined audit export once Sprint 1's status/audit
  windowing lands.
- Replace static grant scope strings with a structured allowlist before remote
  worker execution expands in Sprint 4.
- Gate `POST /api/autonomy-revoke` behind operator auth in the remote execution
  sprint.
