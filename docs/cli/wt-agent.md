# wt-agent

`wt-agent` is the native Phase 0 room participant for the shared JSONL room.
It reads the durable transcript, decides whether the latest CEO/system message
targets the instance, and appends one deterministic stub response.

Phase 0 supports these room agents:

- `claude`
- `chatgpt`
- `gemini`

`qwen` is not started in this native room slice. Qwen remains future adapter
work and should not be enabled as `wt-agent@qwen.service` until the agent binary
explicitly supports it.

## Build

```sh
make build/wt-agent
```

The native room agent has no Redis, SQLite, Jansson, or hiredis dependency.

## Run

One-shot mode:

```sh
bin/wt-agent --agent claude --once --config config/woventeam-phase0.conf
```

Long-running polling mode:

```sh
bin/wt-agent --agent claude --loop --config config/woventeam-phase0.conf
```

`bin/wt-agent` is a launcher that asks `make` whether `build/wt-agent` is
current, rebuilds when needed, and then executes the compiled binary.

## Behavior

- Reads recent messages from `roomLogPath`, defaulting to
  `data/phase0-room.jsonl`.
- Responds only to messages from `ceo` or `system`.
- Responds only when `targetName` is the agent name or `all`.
- Writes one state file per agent beside the room log, such as
  `data/claude.state`, so each triggering message is handled once.
- Uses `agentPollMilliseconds` from config in `--loop` mode.

## systemd

The template service starts each supported agent in loop mode:

```sh
sudo install -m 0644 deploy/systemd/wt-agent@.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now \
  wt-agent@claude.service \
  wt-agent@chatgpt.service \
  wt-agent@gemini.service
```

Use the Makefile helper to install the current template, disable the obsolete
Qwen instance, clear failed state, and start the supported agents:

```sh
make install-agent-services
```

Verify:

```sh
systemctl is-active wt-agent@claude.service
systemctl is-active wt-agent@chatgpt.service
systemctl is-active wt-agent@gemini.service
systemctl is-enabled wt-agent@qwen.service
journalctl -u wt-agent@claude.service -f
```

`wt-agent@.service` has `After=wt-roomd.service` and `Wants=wt-roomd.service`
so boot starts the room daemon before agent loops.

## Validation

```sh
make test-smoke
```
