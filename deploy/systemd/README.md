# systemd deployment for WovenTeam Phase 0

This directory holds systemd units for the native Phase 0 room daemon and for
one `wt-agent` process per supported room model (claude, chatgpt, gemini) on
`xenon`.

## Room daemon

`wt-roomd.service` runs the native HTTP/SSE room daemon. It serves the browser
UI, accepts `/api/message`, streams `/events`, and appends room messages to
`data/phase0-room.jsonl`.

Install and enable:

```sh
sudo install -m 0644 deploy/systemd/wt-roomd.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now wt-roomd.service
```

Verify:

```sh
systemctl status wt-roomd.service
curl -fsS http://127.0.0.1:8787/api/health
journalctl -u wt-roomd.service -f
```

The service uses `bin/wt-roomd`, which rebuilds `build/wt-roomd` when the
binary is missing or stale. The daemon binds according to
`config/woventeam-phase0.conf`; the default Phase 0 config is `0.0.0.0:8787`
for headless intranet access.

## Agent install

```sh
sudo install -m 0644 deploy/systemd/wt-agent@.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now \
  wt-agent@claude.service \
  wt-agent@chatgpt.service \
  wt-agent@gemini.service
```

The Makefile helper installs the current template, disables the obsolete Qwen
instance, clears failed agent states, and starts the supported instances:

```sh
make install-agent-services
```

## Agent boot failure checklist

If agents fail on boot with repeated usage output and `status=2`, check the
installed template:

```sh
systemctl status 'wt-agent@*.service'
journalctl -u wt-agent@claude.service -n 50 --no-pager
systemctl cat wt-agent@claude.service
```

The native Phase 0 room agent must be launched as:

```sh
/woventeam/repos/WovenTeam/bin/wt-agent --agent claude --loop --config /woventeam/repos/WovenTeam/config/woventeam-phase0.conf
```

The older `bin/wt-agent %i` form exits with usage status because the native
agent requires `--agent`. The older Redis dependency is also not part of this
room-mode agent. Apply the repo template with:

```sh
make install-agent-services
```

Expected boot state:

```sh
systemctl is-enabled wt-agent@claude.service wt-agent@chatgpt.service wt-agent@gemini.service
systemctl is-active wt-agent@claude.service wt-agent@chatgpt.service wt-agent@gemini.service
systemctl is-enabled wt-agent@qwen.service  # disabled until Qwen room support exists
```

When multiple agents respond at the same time, `wt_room_store.c` assigns and
appends each message while holding the room-log lock. `make test-smoke`
includes a concurrent-agent check to prevent duplicate `messageId` regression.

## Operational notes

- Logs go to journald under `wt-agent-<NAME>` syslog identifier:
  `journalctl -u wt-agent@claude -f`
- The unit is lightly hardened (`NoNewPrivileges`, `ProtectSystem=full`,
  `ProtectHome=read-only`, `PrivateTmp=yes`) with `ReadWritePaths=/woventeam`
  so the agent can write room logs, state files, and build outputs.
- `Restart=on-failure, RestartSec=5s` — crashes or SIGKILLs come back
  within ~5s.
- The launcher `bin/wt-agent` runs `make -q build/wt-agent` and rebuilds
  on demand, so a clean checkout starts cleanly without a manual build.
