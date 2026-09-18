# t830-slim agent

Stdlib rewrite: chat + SQLite FTS RAG + whitelist tools + markdown skills.
Not Hermes. Not hardware-verified on T830.

## Run

```bash
python3 -S slim/agent.py --help
python3 -S slim/agent.py --ingest-only
python3 -S slim/agent.py --base-url http://127.0.0.1:8080/v1 --model tiny --once 'what is this device'
```

Env: `SLIM_BASE_URL`, `SLIM_FALLBACK_URL`, `SLIM_MODEL`, `SLIM_FALLBACK_MODEL`, `SLIM_API_KEY`, `SLIM_MAX_TOKENS`, `SLIM_TIMEOUT`, `SLIM_DATA_DIR`, `SLIM_SKILLS_DIR`, `SLIM_DOCS_DIR`, `SLIM_MQTT_HTTP`, `SLIM_ALLOW_WRITE`, `SLIM_ALLOW_MQTT`, `SLIM_CONFIG`.

Optional JSON `--config`:
`base_url`, `fallback_url`, `model`, `fallback_model`, `data_dir`, `skills_dir`, `docs_dir`, `mqtt_http`, `allow_write`, `allow_mqtt`.

## Fail-closed tool policy

Two rules, implemented twice so both clients behave the same
(`slim/policy.py` and `slim/cpp/policy.cpp`):

1. **State files are never writable.** `write_file` refuses `*.sqlite`, `*.sqlite3`,
   `*.db` (plus their `-wal`, `-journal`, `-shm` sidecars), dotfiles, and the agent
   binary. Reason: `--db` defaults to `<data_dir>/slim.sqlite` while `write_file` is
   jailed to `<data_dir>`, so a single tool call used to overwrite the agent's own
   database. `data_dir` is scratch space, RAG corpus and state store at once — this
   rule is what stops those three roles from colliding.
2. **Model-initiated mutations are denied by default.** `write_file` and `mqtt_publish`
   require `--allow-write` / `--allow-mqtt` (or `SLIM_ALLOW_WRITE=1` /
   `SLIM_ALLOW_MQTT=1`). With a terminal attached an unapproved mutation prompts
   `[y/N]`; with no terminal it is denied instead of hanging. Read-only tools
   (`rag_search`, `read_file`) are never gated, and **unclassified tools are denied**,
   so a newly added tool cannot become writable by accident.

Every decision is logged to stderr:

```
[slim] tool write_file (notes.md) -> deny: blocked by policy: model-initiated write_file is not enabled
```

Actions a human asks for directly (the `/write`, `/mqtt` slash commands in the C++
client) are not gated, because the human is the approver — rule 1 still applies to them.

## Features

- Chat via OpenAI-compatible HTTP; fallback URL on request failure
- Prompt trimmed to ~6000 chars (~2048-token class)
- RAG: FTS5 keyword search over `slim/skills`, `slim/docs`, `slim/data`
- Tools (one JSON call then a final answer): `rag_search`, `read_file`, `write_file` (jailed, 8 KiB), `mqtt_publish` only if `SLIM_MQTT_HTTP` is set
- Skills: `slim/skills/*.md` injected as text; scripts are not executed
- Session turns stored in `slim/data/slim.sqlite` (protected by the policy above)

## Tests

```bash
python3 -S slim/test_agent.py
python3 -S slim/test_rag.py
python3 -S slim/test_tools.py
python3 -S slim/test_policy.py   # policy + the state-file regression
```

`test_policy.py` includes the regression that matters most: open the default-layout
database, attempt `write_file slim.sqlite`, and assert the database still reads back.

## C++ executable (same features)

Host:

```bash
make -C slim/cpp host
make -C slim/cpp test        # 44 checks: suffix/list/jail/policy/json
./slim/cpp/slim-agent --help
```

T830 (OpenWrt musl gcc 9.3):

```bash
make -C slim/cpp t830
file slim/cpp/slim-agent-t830
# ELF aarch64, interpreter /lib/ld-musl-aarch64.so.1
# NEEDED: libstdc++.so.6 libgcc_s.so.1 libc.so
```

Copy `slim-agent-t830` plus `slim/skills` onto the CPE. Flags: `--base-url`, `--model`,
`--once`, `--data-dir`, `--max-tokens`, and the policy flags `--allow-write`,
`--allow-mqtt`, `--non-interactive` / `--interactive`. The 270M-class local model does
not emit usable tool JSON, so tools are reached through the `/rag`, `/read`, `/write`,
`/mqtt`, `/help` slash commands rather than model tool calls; RAG is a keyword scan of
`.md`/`.txt` (no SQLite in the C++ client). Not hardware-verified on T830.

## Out of scope

Gateway, dashboard, Honcho, embeddings, MCP, arbitrary shell, full Hermes skills, 3B local models.
