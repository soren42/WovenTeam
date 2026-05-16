# Phase 0 Agent Runtime Roadmap

**Status:** Sprint 1 execution plan  
**Date:** 2026-05-16  
**Source:** `docs/reviews/phase0-agent-access-roadmap-2026-05-16.html`

## Current Position

The common room is live and useful as a shared conversation and audit surface.
It is not yet a work-capable orchestration system. The native `wt-agent`
processes are stub participants that read room messages and write deterministic
responses. Harness execution belongs behind explicit adapters, not raw chat
message execution.

## Sprint 1: Reality And Schema

### Goals

1. Make host and harness capability visible.
1. Define the task package contract before adding runtime behavior.
1. Correct profile drift between configured models and installed launchers.
1. Establish a JSONL task ledger path that later code can consume.

### Deliverables

| Deliverable | Path | Exit Criteria |
| --- | --- | --- |
| Task package schema | `docs/api/task-package-v0.1.json` | Role, task, agent, model, context, acceptance criteria, and tool policy are represented in one package. |
| Harness checker | `bin/wt-harness-check` | Reports host harness tools, active model profiles, CLI availability, version probes, and drift. |
| Harness checker tests | `tests/integration/wt-harness-check.sh` | Missing and present CLIs are detected deterministically with fake PATH fixtures, including Claude, Codex, Gemini, Qwen, and Ollama probes. |
| Model profile correction | `models/openai/*.yaml`, `models/google/*.yaml`, `models/qwen/qwen_max.yaml`, `models/xai/grok.yaml` | Primary active profiles point at launchable Claude, Codex, and Gemini harnesses; non-primary profiles without a runtime are standby. |
| Ledger convention | `data/task-packages.jsonl` | Runtime-generated task packages have a defined append-only home and remain git-ignored. |

## Sprint 2: Assignment Path

**Status:** implemented for the Phase 0 stub runtime.

1. Add `wt-task` commands for `create`, `list`, `show`, `assign`, and
   `update-status`.
1. Add `POST /api/task-package` and `GET /api/tasks` to `wt-roomd`.
1. Emit room-visible `task.assign`, `task.status`, and `task.result` events.
1. Teach `wt-agent` to poll task packages by assigned agent and status.

Implementation notes:

- `bin/wt-task` wraps the daemon APIs.
- `wt-roomd` exposes `POST /api/task-package`, `POST /api/task-event`, and
  `GET /api/tasks`.
- `wt-agent` claims queued or assigned packages for its agent name, emits
  `task.status`, then emits a stub `task.result`.
- Harness execution remains Sprint 3 work.

## Sprint 3: First Work Adapter

1. Build a Codex-first adapter behind an explicit feature flag.
1. Run tasks in per-task workspaces under `/woventeam/runtime/tasks/<taskId>`.
1. Capture stdout, stderr, exit code, timeout status, output hash, and artifact
   references.
1. Start with `repo_branch` and `test_local` tool policies only.
1. Add Claude and Gemini adapters after the Codex path proves the ledger,
   workspace, and permission contracts. The Gemini adapter must set an explicit
   trusted-workspace policy for headless execution.

## Sprint 4: Manager-Driven Subtasks

1. Add `task.request` messages for Program Manager and Project Manager roles.
1. Enforce role spawn policy from ROLE.md and role YAML constraints.
1. Add dependency and blocked-state propagation.
1. Verify the CEO-to-PM-to-worker-to-reviewer path with a smoke test.

## UI Coordination Note

The target web app UI design can evolve independently during Sprint 1. Treat it
as presentation target state only until the backend task package and task ledger
contracts are stable.
