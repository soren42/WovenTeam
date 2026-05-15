# Phase 0 Operations

The native room uses `data/phase0-room.jsonl` as the durable system of record.
Each message is one JSON object per line. Agent duplicate-response protection is
stored in `data/claude.state`, `data/chatgpt.state`, and `data/gemini.state`.

Useful commands:

```sh
make
make test-smoke
./build/wt-tail --follow
./build/wt-roomd --config config/woventeam-phase0.conf
WT_FSYNC_EACH_MESSAGE=1 ./build/wt-say ceo all "durable write test"
```

System service:

```sh
sudo install -m 0644 deploy/systemd/wt-roomd.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now wt-roomd.service
systemctl status wt-roomd.service
journalctl -u wt-roomd.service -f
```

Agent services:

```sh
make install-agent-services
systemctl is-active wt-agent@claude.service
systemctl is-active wt-agent@chatgpt.service
systemctl is-active wt-agent@gemini.service
systemctl is-enabled wt-agent@qwen.service
```

The native room agent supports Claude, ChatGPT, and Gemini. Keep
`wt-agent@qwen.service` disabled until a Qwen room adapter is implemented.
Concurrent agent appends are serialized with a room-log file lock so each
response receives a unique `messageId`.
The install helper restarts the supported services after installing the unit so
live agents pick up rebuilt binaries immediately.

The HTTP server binds to `0.0.0.0` by default for headless intranet access.
Keep this service behind the local firewall and do not expose it directly to
the public Internet until authentication, TLS, and operator controls exist.

The Phase 0 core does not execute commands from room messages. Vendor CLI
integration belongs behind explicit adapter code in a later Phase 0.5 step.
