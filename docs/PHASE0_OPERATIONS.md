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

The HTTP server binds to `0.0.0.0` by default for headless intranet access.
Keep this service behind the local firewall and do not expose it directly to
the public Internet until authentication, TLS, and operator controls exist.

The Phase 0 core does not execute commands from room messages. Vendor CLI
integration belongs behind explicit adapter code in a later Phase 0.5 step.
