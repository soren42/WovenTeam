# WovenTeam
Solarian WovenTeam Multi-Agentic AI Stack

[Website](https://solariangroup.com/woventeam) | [Docs](https://solariangroup.com/woventeam/docs) | [Issues](https://github.com/soren42/WovenTeamv/issues)

[![Open Source](https://img.shields.io/badge/Open%20Source-%E2%9D%A4-red.svg)]()
[![License](https://www.gnu.org/graphics/gplv3-or-later-sm.png)](https://solariangroup.com/license/GPL/)

[![Twitter Follow](https://img.shields.io/twitter/follow/soren42?style=social)](https://twitter.com/soren42)

## Phase 0 Native Room

The current Phase 0 spike is a native GNU/Linux C communication room. It avoids
Docker, Redis, SQLite, Node, Python, PHP, and framework runtimes in the core.
The durable system of record is one append-only JSON Lines file:

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
make DEBUG=1
make install-roomd-service
make install-agent-services
```

Check host harness visibility against active model profiles:

```sh
./bin/wt-harness-check
./bin/wt-harness-check --json --strict
```

The first structured orchestration contract is documented as JSON Schema in
`docs/api/task-package-v0.1.json`. Runtime task packages are expected to land in
the append-only `data/task-packages.jsonl` ledger, described in
`docs/api/task-ledger-v0.1.md`, until the project needs a queryable store.

Create and inspect Phase 0 task packages through the room daemon:

```sh
./bin/wt-task create --title "Verify assignment path" --body "Claim and complete this stub task." --role backend_dev --agent chatgpt --max-tokens 2000000
./bin/wt-task list
./bin/wt-task show task_example_001
./bin/wt-task assign task_example_001 --agent gemini
./bin/wt-task update-status task_example_001 --status blocked --message "Waiting on operator input."
```

The web console token panel is backed by `GET /api/tokens`. Phase 0 reports
allocated token budget from task packages, not adapter-measured usage. Token
budgets and estimated cost settings live in `config/woventeam-phase0.conf` and
can be edited from the console settings rail.

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
