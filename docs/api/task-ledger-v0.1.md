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
    "maxCostUsd": 1.0,
    "maxTokens": 2000000
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

Manager-driven subtasks append an explicit request record before the generated
child package:

```json
{
  "schema": "woventeam.task_request.v0.1",
  "taskId": "task_child_001",
  "parentTaskId": "task_parent_001",
  "requestedTaskId": "task_child_001",
  "initiativeId": "init_example",
  "requestedBy": "project_manager",
  "requestedByRole": "project_manager",
  "requestedRole": "backend_dev",
  "assignedAgent": "chatgpt",
  "title": "Implement a small backend change",
  "body": "Make the requested code change.",
  "toolPolicy": {
    "profile": "observe"
  },
  "createdAtUnixMs": 1778917466000
}
```

`wt-roomd` validates the request against the Phase 0 spawn policy, appends the
request, emits a `task.request` room message, appends the generated child task
package, and emits `task.assign`. The child package includes `parentTaskId`,
`requestedByRole`, and a dependency on the parent task.

`wt-roomd` exposes the Phase 0 ledger through:

- `POST /api/task-package`
- `POST /api/task-request`
- `POST /api/task-event`
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
- `POST /api/config`
- `POST /api/task-gate`
- `POST /api/task-usage`
- `POST /api/agent-control`
- `POST /api/task-reclaim`
- `POST /api/task-artifact`
- `GET /api/initiative-artifacts?initiativeId=...`
- `GET /api/initiative-audit?initiativeId=...`

`bin/wt-task` wraps those endpoints for local operator use with `create`,
`request`, `list`, `show`, `assign`, `update-status`, `retry`, `cancel`,
`close`, `reopen`, `reclaim`, `initiative create/list/show/export/close`,
`agent list/pause/resume`,
`artifact promote/reject/review/supersede/note/list/export`, and
`audit INIT [--out FILE]` commands.

Phase 1 keeps the JSONL ledger as the recovery source and adds a rebuildable
SQLite projection configured by `taskProjectionDbPath`. `wt-roomd` rebuilds the
projection at startup and before projected reads. `GET /api/task-summaries`
returns the current task rows from that projection, and
`GET /api/task-detail?taskId=...` returns one task plus its projected event
timeline. `GET /api/task-artifacts?taskId=...` exposes the adapter workspace
file list and previews of `result.md`, `stdout.log`, `stderr.log`, and
`manifest.json`. If the SQLite file is lost, it can be deleted and regenerated
from `data/task-packages.jsonl`.

Phase 2 derives initiatives from task package `initiativeId` values. `GET
/api/initiatives` returns initiative summaries with task counts, active task
counts, blocked/open-gate counts, token allocation, and timestamps. `GET
/api/initiative-detail?initiativeId=...` returns the same summary plus the
projected task rows for that initiative. There is no separate durable
initiative table yet; the task ledger remains authoritative.

`GET /api/tokens` reports token allocation from task package budgets and actual
reported usage from `woventeam.task_usage.v0.1` records. Allocation sums
`budget.maxTokens` over rolling 24-hour and 30-day windows and is enforced as a
hard stop before new task packages or manager subtasks are accepted.

Usage events use:

```json
{
  "schema": "woventeam.task_usage.v0.1",
  "taskId": "task_example_001",
  "provider": "openai",
  "modelId": "openai/gpt-5.3-codex",
  "inputTokens": 100,
  "outputTokens": 250,
  "totalTokens": 350,
  "estimatedCostCents": 7,
  "createdBy": "wt-agent@chatgpt",
  "createdAtUnixMs": 1778917466000
}
```

`GET /api/capacity` reports active task counts by agent, initiative, and parent
task. `wt-roomd` uses the same projection-backed counts to enforce
`maxActiveTasksPerAgent`, `maxSubtasksPerParent`, and `maxTasksPerInitiative`
when routing new task packages or manager subtasks.

`POST /api/task-gate` records operator review decisions as append-only task
events with `eventType=review_gate`. Supported actions are `approve`, `reject`,
and `revision`.

Phase 2 workload control adds task lease events and agent control records. Before
`wt-agent` starts work, it appends a normal task event with
`eventType=lease`, `status=leased`, `assignedAgent`, `attempt`, and
`leaseExpiresAtUnixMs`. The projection exposes `attemptCount`, `leaseOwner`,
`leasedAtUnixMs`, `runningAtUnixMs`, and `leaseExpiresAtUnixMs` on task summary
and detail rows.

Agent pause/resume records use:

```json
{
  "schema": "woventeam.agent_control.v0.1",
  "agent": "chatgpt",
  "action": "pause",
  "message": "Operator pause.",
  "createdBy": "ceo",
  "createdAtUnixMs": 1779205447000
}
```

`GET /api/agents` returns one row for each primary agent with active, leased,
running, stuck, and attempt counts. A task is considered stuck when it remains
`leased` or `running` for more than 15 minutes.

Sprint 3 closeout (2026-05-20) adds two more event shapes. A `reclaim` task
event releases the most recent lease and sends the task back to the queued
pool:

```json
{
  "schema": "woventeam.task_event.v0.1",
  "taskId": "task_example_001",
  "eventType": "reclaim",
  "status": "queued",
  "assignedAgent": "chatgpt",
  "reclaimReason": "lease_expired",
  "message": "Auto-reclaim after lease expired (previous holder=chatgpt).",
  "createdBy": "wt-agent@gemini",
  "createdAtUnixMs": 1779249000000
}
```

`reclaimReason` is an allowlist of `operator` (operator-triggered) or
`lease_expired` (recorded automatically by `wt-agent` when it finds a stuck
foreign lease). Failed status events may additionally carry a `retryCause`
field:

```json
{
  "schema": "woventeam.task_event.v0.1",
  "taskId": "task_example_001",
  "eventType": "status",
  "status": "failed",
  "message": "Codex adapter exitCode=124 timedOut=true; see task workspace manifest.",
  "retryCause": "timeout",
  "createdBy": "wt-agent@chatgpt",
  "createdAtUnixMs": 1779249000000
}
```

`retryCause` values emitted by `wt-agent` adapters: `timeout` (timeout kill),
`adapter_unavailable` (exec returned 127), `exit_nonzero` (any other non-zero
exit). The projection surfaces the latest classified cause on each task as
`failureCause`, the most recent reclaim's reason as `lastReclaimReason`, and a
cumulative `reclaimCount`. `POST /api/task-reclaim` accepts a JSON body with
`taskId`, `reason` (`operator` or `lease_expired`), optional `message`, and
optional `createdBy`; it appends the same `reclaim` task event a stuck-task
recovery would emit.

When a task is marked `blocked`, queued/running child tasks that list it in
`dependencies` receive an appended `blocked` task event. Terminal dependents are
not rewritten.

Sprint 4 (2026-05-20) adds artifact promotion. Each operator decision about an
adapter-produced workspace artifact is recorded as a task event with
`eventType=artifact` and an `artifactState` field. Supported states are
`draft`, `reviewed`, `accepted`, `rejected`, and `superseded`:

```json
{
  "schema": "woventeam.task_event.v0.1",
  "taskId": "task_example_001",
  "eventType": "artifact",
  "artifactState": "accepted",
  "reviewer": "ceo",
  "reviewNotes": "Ships this iteration. Passed manual smoke.",
  "artifactPath": "result.md",
  "message": "Ships this iteration. Passed manual smoke.",
  "createdBy": "ceo",
  "createdAtUnixMs": 1779251400000
}
```

The projection latches `artifactState`, `lastReviewer`, and `lastReviewNotes`
on every artifact event. The `accepted_at_ms` and `accepted_artifact_path`
columns latch only when state is `accepted`, so the inventory keeps a stable
record of "what was accepted, by whom, and when" even after later supersede
events.

`POST /api/task-artifact` accepts a JSON body with `taskId`, `state`
(allowlisted to the five states above), optional `reviewer`,
optional `notes`, and optional `artifactPath` (workspace-relative; rejected if
it contains traversal characters). The endpoint appends the artifact task event
and broadcasts a `task.artifact` room message so chat observers see promotion
activity inline.

`GET /api/initiative-artifacts?initiativeId=...` returns the per-initiative
inventory of artifact decisions, sorted with accepted assets first:

```json
{
  "ok": true,
  "initiativeId": "init_example",
  "artifacts": [
    {
      "taskId": "task_example_001",
      "title": "Implement adapter manifest dump",
      "assignedAgent": "claude",
      "artifactState": "accepted",
      "lastReviewer": "ceo",
      "lastReviewNotes": "Ships this iteration.",
      "acceptedAtUnixMs": 1779251400000,
      "updatedAtUnixMs": 1779251400000,
      "acceptedArtifactPath": "result.md"
    }
  ],
  "acceptedCount": 1,
  "pendingCount": 0
}
```

Codex adapter runs also write an artifact manifest under the per-task runtime
workspace. The manifest uses:

```json
{
  "schema": "woventeam.adapter_manifest.v0.1",
  "taskId": "task_example_001",
  "adapter": "codex",
  "command": "codex",
  "workspace": "/woventeam/runtime/tasks/task_example_001",
  "stdout": "/woventeam/runtime/tasks/task_example_001/stdout.log",
  "stderr": "/woventeam/runtime/tasks/task_example_001/stderr.log",
  "result": "/woventeam/runtime/tasks/task_example_001/result.md",
  "timedOut": false,
  "exitCode": 0
}
```

This keeps the Phase 0 storage model consistent with the room transcript:
append-only, inspectable, and easy to recover after process restarts. The
SQLite projection indexes this ledger without changing the package contract.

Sprint 5 (2026-05-20) adds policy and audit. Three new config keys drive the
central policy evaluator:

- `blockedVendors` — comma-separated list of vendor prefixes (matched against
  the family before the first `/` in `modelId`). Default: `deepseek` (existing
  trust-boundary decision documented in `/woventeam/docs/audit/`).
- `tokenBudgetPerInitiative` — cap on the sum of `budget.maxTokens` for active
  (non-terminal) task packages within a single `initiativeId`. `0` disables.
- `tokenBudgetPerModelFamily` — cap on the sum of `budget.maxTokens` for active
  task packages whose `modelId` starts with `<family>/`. `0` disables.

When the evaluator denies a request it appends a durable record:

```json
{
  "schema": "woventeam.policy_decision.v0.1",
  "taskId": "task_example_001",
  "initiativeId": "init_example",
  "decision": "deny",
  "reason": "vendor_blocked",
  "message": "vendor 'deepseek' is on the blockedVendors allowlist (modelId=deepseek/coder).",
  "createdBy": "ceo",
  "createdAtUnixMs": 1779253511752
}
```

Reason codes (stable; consumed by both the CLI and the web console):
`vendor_blocked`, `model_agent_mismatch`, `initiative_budget`,
`model_family_budget`, `daily_budget`, `monthly_budget`, `capacity_agent`,
`capacity_initiative`, `autonomy_required`, `autonomy_expired`,
`autonomy_revoked`.

## Phase 3 autonomy contract

Sprint 2 of the v1.0 phase adds bounded autonomy grants to task packages. A
package may include:

```json
{
  "autonomyLevel": "autonomous",
  "autonomyGrant": {
    "scope": "workspace",
    "ttlSeconds": 3600,
    "maxWallClockSeconds": 1800,
    "maxCostUsd": 1.0,
    "maxTokens": 2000000,
    "network": "intranet",
    "credentialClass": "repo-write",
    "requiresCleanWorktree": false
  }
}
```

`autonomyLevel` may be `observe`, `ask-each`, `ask-batch`, or `autonomous`.
When omitted, the daemon derives a default from `toolPolicy.profile`, unless a
per-agent config default overrides it. Autonomous execution requires a positive
`ttlSeconds` and a scope containing `workspace`, `adapter`, or `*`; otherwise
the policy evaluator rejects the package with `autonomy_required`. Expired
grants are rejected as `autonomy_expired`.

Every elevated adapter invocation and every adapter invocation decision appends:

```json
{
  "schema": "woventeam.autonomy_event.v0.1",
  "taskId": "task_example_001",
  "actor": "wt-agent@chatgpt",
  "target": "codex",
  "action": "adapter_invocation_elevated",
  "commandClass": "codex-cli",
  "autonomyLevel": "autonomous",
  "reason": "autonomy grant permits codex adapter invocation",
  "allowed": true,
  "exitCode": -1,
  "createdAtUnixMs": 1779253511752
}
```

`POST /api/autonomy-revoke` accepts `taskId`, optional `reason`, and optional
`createdBy`. It appends `woventeam.autonomy_event.v0.1` with
`action:"revoked"` and a `woventeam.kill_event.v0.1` with
`reason:"autonomy-revoke"`. The next adapter invocation downgrades to normal
approval flags after revocation.

`GET /api/initiative-audit?initiativeId=...` returns a single combined report:

```json
{
  "ok": true,
  "initiativeId": "init_example",
  "summary": {
    "taskCount": 7,
    "activeTasks": 2,
    "completeTasks": 4,
    "failedTasks": 1,
    "blockedTasks": 0,
    "acceptedArtifacts": 3,
    "maxTokens": 14000,
    "createdAtUnixMs": 1779000000000,
    "updatedAtUnixMs": 1779253500000,
    "title": "Implementation initiative"
  },
  "tasks": [ /* full projection rows */ ],
  "events": [ /* every task_event row in chronological order */ ],
  "policyDecisions": [ /* every denial, including ones for rejected taskIds */ ],
  "usage": [ /* every woventeam.task_usage.v0.1 record for these tasks */ ]
}
```

Empty initiatives return HTTP 404 with `{"ok": false, "error": "initiative not
found"}`. The `wt-task audit INIT [--out FILE]` CLI streams the same payload
to stdout (or to a file with `--out`).

## Phase 3 deliverables pipeline

Sprint 3 of the v1.0 phase defines where accepted work product lands. An
accepted artifact (its `accepted_artifact_path` row in the projection) can be
shipped through `POST /api/task-deliverable` or `wt-task artifact ship`,
which copies, packages, or publishes the file out of the per-task workspace
into a stable per-initiative `deliverableRoot` (default
`data/deliverables/<initiativeId>/`).

Each ship appends a `woventeam.deliverable.v0.1` record:

```json
{
  "schema": "woventeam.deliverable.v0.1",
  "deliverableId": "deliv_018f6a...",
  "taskId": "task_example_001",
  "initiativeId": "init_example",
  "sourceWorkspacePath": "/woventeam/runtime/task_example_001/result.md",
  "deliverablePath": "data/deliverables/init_example/result.md",
  "packagingMode": "copy",
  "sizeBytes": 8421,
  "sha256": "9a0b8c...",
  "reviewer": "operator",
  "supersedes": null,
  "autonomyProvenance": {
    "elevated": false,
    "autonomyLevel": "ask-each"
  },
  "createdBy": "operator",
  "createdAtUnixMs": 1779260000000
}
```

`packagingMode` is one of:

- `copy` — straight file copy into the deliverable root. Default.
- `tarball` — gzipped tarball under the deliverable root; `deliverablePath`
  points at the tarball, and the manifest records `sourceWorkspacePath`
  for the original.
- `branch` — commit the file to a `deliverables/<initiativeId>` branch in
  the project repo and push. Requires a clean worktree on that branch.
- `pull-request` — same as `branch`, then opens a PR via `gh`. Operator-gated
  by an explicit `"yes": true` (or `--yes` on the CLI); refuses otherwise.

The `supersedes` field, if non-null, points to a prior `deliverableId`. The
ledger and projection preserve the predecessor's `sha256` + `createdAtUnixMs`
so audit consumers can trace a deliverable's revision history.

`autonomyProvenance` is filled in at ship time by looking up the most recent
`woventeam.autonomy_event.v0.1` for `taskId`. It records whether the task
ran under elevation and what `autonomyLevel` was in effect. The field is
optional; if no autonomy_event exists for the task, the daemon omits it.

### Pre-ship secret scan

`branch` and `pull-request` modes refuse to ship if the staged content matches
any of the configured `secretScanPatterns` (regex strings; defaults cover
GitHub PATs, AWS keys, generic high-entropy `AKIA…` patterns, OpenSSH private
keys, and a small set of API-token shapes). The scan result lands as:

```json
{
  "schema": "woventeam.secret_scan.v0.1",
  "scanId": "scan_018f6a...",
  "deliverableId": "deliv_018f6a...",
  "taskId": "task_example_001",
  "scannedPath": "/woventeam/runtime/task_example_001/result.md",
  "matched": true,
  "hitCount": 2,
  "patternHits": [
    {"pattern": "github-pat", "count": 1},
    {"pattern": "aws-akia",   "count": 1}
  ],
  "packagingMode": "branch",
  "createdAtUnixMs": 1779260000050
}
```

Scans run on `copy` and `tarball` too — they record but do not block, so the
audit trail has a record on every ship. Only `branch` and `pull-request`
block on a positive match (with policy reason `secret_scan_block`).

### Audit export

`GET /api/initiative-audit` gains two arrays alongside the existing ones:

```json
{
  "deliverables": [ /* every woventeam.deliverable.v0.1 row */ ],
  "secretScans":  [ /* every woventeam.secret_scan.v0.1 row */ ]
}
```

The arrays honor the same `?since=<ms>&limit=<N>` cursor as the base export.
Older audit consumers that don't know about these fields ignore them; the
schema is additive.
