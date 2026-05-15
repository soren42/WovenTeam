# Phase 0 Acceptance Test

Run from the repository root:

```sh
make clean
make
cp config/woventeam-phase0.conf.example config/woventeam-phase0.conf
./build/wt-roomd --config config/woventeam-phase0.conf
```

In separate terminals:

```sh
./build/wt-agent --agent claude --loop
./build/wt-agent --agent chatgpt --loop
./build/wt-agent --agent gemini --loop
```

Then send a CEO message and inspect the shared room:

```sh
./build/wt-say ceo all "Claude, ChatGPT, Gemini: each confirm that you can read and respond through the shared room."
./build/wt-tail --limit 20
```

Expected result:

- the CEO message is appended to `data/phase0-room.jsonl`;
- each stub agent appends one deterministic response;
- `http://HOSTNAME_OR_IP:8787/` shows the same messages from an intranet browser;
- no Docker, Redis, SQLite, Node, Python, PHP, or CDN is required for the core.
