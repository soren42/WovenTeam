# systemd deployment for WovenTeam Phase 0

This directory holds systemd units for the native Phase 0 room daemon and for
one `wt-agent` process per peer model (claude, chatgpt, gemini, qwen) on
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
  wt-agent@gemini.service \
  wt-agent@qwen.service
```

Each instance subscribes to Redis `wt:tasks:new` and filters by
`payload.assigned_to` matching its `%i` instance name.

## Required Redis config

When an agent is killed abruptly (SIGKILL, host crash, network drop), its
TCP socket to Redis is not closed cleanly. Without keepalive, the stale
connection lingers indefinitely as a phantom pub/sub subscriber and
`redis-cli PUBSUB NUMSUB wt:tasks:new` will overcount.

Set Redis-side keepalive to 60s so the kernel starts probing dead
connections after 60s of idle time:

```sh
# Live (applies to new connections):
sudo redis-cli CONFIG SET tcp-keepalive 60
sudo redis-cli CONFIG REWRITE   # persists into /etc/redis/redis.conf

# Force existing connections to reconnect with keepalive enabled:
sudo systemctl restart 'wt-agent@*.service'
```

Verify:

```sh
redis-cli CONFIG GET tcp-keepalive   # → tcp-keepalive 60
redis-cli PUBSUB NUMSUB wt:tasks:new # → 4 (one per active agent)
```

**Timing note.** Redis's `tcp-keepalive` only sets `TCP_KEEPIDLE` (the
idle threshold). The probe interval (`TCP_KEEPINTVL`, default 75s) and
probe count (`TCP_KEEPCNT`, default 9) come from kernel sysctl. With
defaults that means a dead connection is reaped roughly 60s + 75×9 =
~12 minutes after it goes silent. That is good enough to prevent
indefinite zombie subscribers but slow for interactive debugging. For
faster reaping on a dev/CI host:

```sh
sudo sysctl -w net.ipv4.tcp_keepalive_intvl=10
sudo sysctl -w net.ipv4.tcp_keepalive_probes=3
# Total reap = ~60s idle + 10s × 3 probes = ~90s
```

To force-clear stale subscribers immediately:

```sh
# Identify by remote port not matching any live agent's redis socket.
redis-cli CLIENT LIST TYPE pubsub
sudo redis-cli CLIENT KILL ID <stale-id>
```

## Operational notes

- Logs go to journald under `wt-agent-<NAME>` syslog identifier:
  `journalctl -u wt-agent@claude -f`
- The unit is lightly hardened (`NoNewPrivileges`, `ProtectSystem=full`,
  `ProtectHome=read-only`, `PrivateTmp=yes`) with `ReadWritePaths=/woventeam`
  so the worker can write to SQLite at `/woventeam/woventeam.db` and to
  `/woventeam/logs`.
- `Restart=on-failure, RestartSec=5s` — crashes or SIGKILLs come back
  within ~5s.
- The launcher `bin/wt-agent` runs `make -q build/wt-agent` and rebuilds
  on demand, so a clean checkout starts cleanly without a manual build.
