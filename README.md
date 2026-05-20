# WovenTeam
Solarian WovenTeam Multi-Agentic AI Stack

[Website](https://solariangroup.com/woventeam) | [Docs](https://solariangroup.com/woventeam/docs) | [Issues](https://github.com/soren42/WovenTeamv/issues)

[![Open Source](https://img.shields.io/badge/Open%20Source-%E2%9D%A4-red.svg)]()
[![License](https://www.gnu.org/graphics/gplv3-or-later-sm.png)](https://solariangroup.com/license/GPL/)

[![Twitter Follow](https://img.shields.io/twitter/follow/soren42?style=social)](https://twitter.com/soren42)

## Phase 0 Native Room

The current Phase 0/1 native room is a GNU/Linux C communication and task
control plane. It avoids Docker, Redis, Node, Python, PHP, and framework
runtimes in the core. The durable audit source remains one append-only JSON
Lines file:

```text
data/phase0-room.jsonl
```

Build:

```sh
make clean
make
```

Create local config and start the room daemon:

```sh
cp config/woventeam-phase0.conf.example config/woventeam-phase0.conf
./build/wt-roomd --config config/woventeam-phase0.conf
```

In separate terminals, start stub agents:

```sh
./build/wt-agent --agent claude --loop
./build/wt-agent --agent chatgpt --loop
./build/wt-agent --agent gemini --loop
```

Send a CEO message and inspect the transcript:

```sh
./build/wt-say ceo all "Claude, ChatGPT, Gemini: each confirm that you can read and respond through the shared room."
./build/wt-tail --limit 20
```

Open the browser UI from another intranet machine at `http://HOSTNAME_OR_IP:8787/`.
For local checks on the server itself, `http://127.0.0.1:8787/` still works.
The served UI is the fullscreen Phase 0 console scaffold documented in
`docs/ui/phase0-console-scaffold.md`.

Useful targets:

```sh
make run-demo
make harness-check
make test-smoke
make test-harness-check
make test-phase1-e2e
make DEBUG=1
make install-roomd-service
make install-agent-services
```

Check host harness visibility against active model profiles:

```sh
./bin/wt-harness-check
./bin/wt-harness-check --json --strict
./bin/wt-adapter-preflight
./bin/wt-adapter-preflight --json
```

The first structured orchestration contract is documented as JSON Schema in
`docs/api/task-package-v0.1.json`. Runtime task packages are expected to land in
the append-only `data/task-packages.jsonl` ledger, described in
`docs/api/task-ledger-v0.1.md`. Phase 1 adds a rebuildable SQLite projection at
`data/task-projection.sqlite` for task summaries and task detail queries.

Create and inspect Phase 0 task packages through the room daemon:

```sh
./bin/wt-task create --title "Verify assignment path" --body "Claim and complete this stub task." --role backend_dev --agent chatgpt --max-tokens 2000000
./bin/wt-task initiative create --title "Launch a bounded initiative" --body "Create the manager charter task." --id init_example
./bin/wt-task initiative list
./bin/wt-task initiative show init_example
./bin/wt-task agent list
./bin/wt-task agent pause chatgpt --message "Hold Codex work while reviewing output."
./bin/wt-task agent resume chatgpt
./bin/wt-task list
./bin/wt-task show task_example_001
./bin/wt-task assign task_example_001 --agent gemini
./bin/wt-task update-status task_example_001 --status blocked --message "Waiting on operator input."
./bin/wt-task retry task_example_001
./bin/wt-task cancel task_example_001
./bin/wt-task close task_example_001
./bin/wt-task reclaim task_example_001 --reason operator --message "Manual unblock - stuck after restart."
./bin/wt-task artifact promote task_example_001 --path result.md --reviewer ceo --notes "Ships."
./bin/wt-task artifact list init_example
./bin/wt-task artifact export task_example_001 --out /tmp/promoted.md
./bin/wt-task audit init_example --out /tmp/audit.json
./bin/wt-rehearse-live --agent claude --max-tokens 4000   # dry-run; add --yes to execute
```

The web console token panel is backed by `GET /api/tokens`. Phase 1 separates
allocated token budget from task packages and actual usage reported through
`POST /api/task-usage`. Token budgets and estimated cost settings live in
`config/woventeam-phase0.conf` and can be edited from the console settings rail.

Program and Project Manager roles can request subtasks through the same daemon.
The daemon records a `task.request`, validates the role spawn policy, creates
the child task package, and links it to the parent task as a dependency:

```sh
./bin/wt-task request \
  --parent task_example_001 \
  --by-role project_manager \
  --role backend_dev \
  --agent chatgpt \
  --title "Implement a scoped change" \
  --body "Produce the worker artifact and report the result."
```

Manager-driven subtasks are documented in
`docs/orchestration/manager-driven-subtasks.md`.
Initiative summary and detail APIs are documented in
`docs/api/task-ledger-v0.1.md`.

After starting `wt-agent` with `WT_ENABLE_CODEX_ADAPTER=1`, create a Codex-
eligible task for an isolated task workspace:

```sh
./bin/wt-task create \
  --title "Draft implementation note" \
  --body "Write a short implementation note in the task workspace." \
  --role backend_dev \
  --agent chatgpt \
  --tool-profile repo_branch \
  --model openai/gpt-5.3-codex
```

Adapter behavior and artifacts are documented in `docs/adapters/codex-adapter.md`.
Task workspaces are visible through `GET /api/task-artifacts?taskId=...` and
the web console task detail artifact viewer.
The shared Phase 1 adapter contract, including the opt-in Claude and Gemini
artifact adapters, is documented in `docs/adapters/adapter-contract-v0.1.md`.
Sprint 3 routing, capacity, and review gate behavior is documented in
`docs/orchestration/routing-and-gates.md`.
Sprint 4 quota and operations behavior is documented in
`docs/ops/phase1-operations-runbook.md`.
The Phase 1 final whole-path validation is documented in
`docs/launch/phase1-spike6-e2e-2026-05-19.md` and can be run with
`make test-phase1-e2e`.
