# Codex Adapter

The Phase 0 Codex adapter is the first work-capable harness runner for
`wt-agent`. It is intentionally opt-in and isolated.

The shared Phase 1 adapter contract is documented in
`docs/adapters/adapter-contract-v0.1.md`.

## Enablement

The adapter only runs when all of these are true:

- `enableCodexAdapter=1` in config, or `WT_ENABLE_CODEX_ADAPTER=1`
- the task is assigned to `chatgpt`
- the task has `toolPolicy.profile` set to `repo_branch` or `test_local`
- `modelId` is empty or starts with `openai/`

Otherwise `wt-agent` keeps the Sprint 2 stub-completion behavior.

## Runtime Workspace

Each adapter task gets an isolated workspace:

```text
/woventeam/runtime/tasks/<taskId>/
```

The runner writes:

- `prompt.md` - prompt assembled from the task package
- `stdout.log` - Codex stdout, capped by `adapterMaxOutputBytes`
- `stderr.log` - Codex stderr, capped by `adapterMaxOutputBytes`
- `result.md` - final Codex message from `--output-last-message`
- `manifest.json` - command path, workspace, artifact paths, timeout state, and exit code
- `manifest.json` - command path, workspace, artifact paths, tool profile,
  timeout, output cap, timeout state, and exit code

The adapter invokes:

```sh
codex exec \
  --ephemeral \
  --skip-git-repo-check \
  --cd /woventeam/runtime/tasks/<taskId> \
  --sandbox workspace-write \
  --ask-for-approval never \
  --output-last-message /woventeam/runtime/tasks/<taskId>/result.md \
  "<assembled prompt>"
```

## Ledger Events

The adapter appends:

- `running` when the Codex process starts
- `complete` when Codex exits with code `0`
- `failed` when Codex exits non-zero or times out

Room-visible events are also emitted as `task.status` and `task.result`.

## Safety Boundaries

The first adapter does not run inside the repository checkout. It proves the
ledger, workspace, timeout, output capture, and manifest contract before a
future Sprint extends adapters to branch-based repository work.
