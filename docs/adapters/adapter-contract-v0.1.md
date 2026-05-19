# Adapter Contract v0.1

Phase 1 adapters turn a task package into controlled work in a per-task
workspace. JSONL remains the audit source; adapter manifests and task events
make execution inspectable from the web UI and CLI.

## Eligibility

An adapter may run only when:

- the task package is assigned to the running agent;
- the task status is `queued` or `assigned`;
- the adapter is explicitly enabled in config or environment;
- the task model and tool profile match the adapter's supported policy.

Current adapters:

| Agent | Adapter | Enablement | Profiles |
| --- | --- | --- | --- |
| `chatgpt` | `codex` | `enableCodexAdapter=1` | `repo_branch`, `test_local` |
| `claude` | `claude` | `enableClaudeAdapter=1`, `claudeMode=adapter` | `observe`, `ops_read` |
| `gemini` | `gemini` | `enableGeminiAdapter=1`, `geminiMode=adapter` | `observe`, `ops_read` |

## Workspace

Each adapter receives:

```text
<runtimeRootPath>/<taskId>/
```

The workspace must contain:

- `prompt.md` - assembled prompt handed to the harness.
- `stdout.log` - raw stdout, capped by `adapterMaxOutputBytes`.
- `stderr.log` - raw stderr, capped by `adapterMaxOutputBytes`.
- `result.md` - operator-readable result artifact.
- `manifest.json` - execution metadata.

## Manifest

`manifest.json` uses:

```json
{
  "schema": "woventeam.adapter_manifest.v0.1",
  "taskId": "task_example",
  "adapter": "codex",
  "command": "codex",
  "toolProfile": "repo_branch",
  "timeoutSeconds": 1800,
  "maxOutputBytes": 1048576,
  "workspace": "/woventeam/runtime/tasks/task_example",
  "prompt": "/woventeam/runtime/tasks/task_example/prompt.md",
  "stdout": "/woventeam/runtime/tasks/task_example/stdout.log",
  "stderr": "/woventeam/runtime/tasks/task_example/stderr.log",
  "result": "/woventeam/runtime/tasks/task_example/result.md",
  "timedOut": false,
  "exitCode": 0
}
```

Codex writes the final message directly to `result.md`.

Claude uses non-interactive print mode:

```sh
claude --bare --print "<assembled prompt>"
```

Gemini uses headless prompt mode:

```sh
gemini --skip-trust --approval-mode plan --output-format text --prompt "<assembled prompt>"
```

The Claude/Gemini artifact adapters copy stdout into `result.md`.

## Events

Adapters append task events:

- `running` when the harness starts.
- `complete` when the harness exits with code `0`.
- `failed` when the harness exits non-zero or times out.

Room-visible `task.status` and `task.result` messages mirror those task events.

## Capability API

`GET /api/adapters` returns adapter enablement, mode, command, resolved command
path, launchability, and supported tool profiles. It is a runtime capability
report, not a security boundary.

Phase 2 adds a stricter `preflight` object to each adapter entry. The top-level
`state` field remains command launchability for backward compatibility, while
`preflight.ok` is the launch-era readiness gate for real work. Preflight checks:

- adapter enablement;
- required adapter mode for Claude and Gemini;
- command executable resolution;
- runtime root or parent writability;
- positive adapter timeout;
- positive output cap.

Example:

```json
{
  "agent": "claude",
  "adapter": "claude",
  "enabled": true,
  "mode": "adapter",
  "command": "/usr/local/bin/claude",
  "commandPath": "/usr/local/bin/claude",
  "state": "launchable",
  "profiles": ["observe", "ops_read"],
  "preflight": {
    "ok": true,
    "state": "ready",
    "reason": "ready",
    "runtimeRootPath": "/woventeam/runtime/tasks",
    "checks": {
      "enabled": true,
      "modeReady": true,
      "commandExecutable": true,
      "runtimeWritable": true,
      "timeoutConfigured": true,
      "outputCapConfigured": true
    },
    "lastFailure": {
      "class": "",
      "message": ""
    }
  }
}
```

`lastFailure` is derived from recent failed task events created by the matching
`wt-agent@...` adapter. Current classes are `missing_cli`, `timeout`,
`nonzero_exit`, and `adapter_failed`.

CLI operators can inspect the same report with:

```sh
./bin/wt-adapter-preflight
./bin/wt-adapter-preflight --json
```

Service deployments should use absolute command paths when a CLI is installed
outside systemd's default PATH. The capability report resolves commands from the
daemon process environment, which may differ from an interactive shell.
