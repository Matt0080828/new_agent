# t830-slim agent

Stdlib rewrite: chat + SQLite FTS RAG + whitelist tools + markdown skills.
Not Hermes. Not hardware-verified on T830.

## Run

```bash
python3 -S slim/agent.py --help
python3 -S slim/agent.py --ingest-only
python3 -S slim/agent.py --base-url http://127.0.0.1:8080/v1 --model tiny --once 'what is this device'
```

Env: `SLIM_BASE_URL`, `SLIM_FALLBACK_URL`, `SLIM_MODEL`, `SLIM_FALLBACK_MODEL`, `SLIM_API_KEY`, `SLIM_MAX_TOKENS`, `SLIM_TIMEOUT`, `SLIM_DATA_DIR`, `SLIM_SKILLS_DIR`, `SLIM_DOCS_DIR`, `SLIM_MQTT_HTTP`, `SLIM_CONFIG`.

Optional JSON `--config`:
`base_url`, `fallback_url`, `model`, `fallback_model`, `data_dir`, `skills_dir`, `docs_dir`, `mqtt_http`.

## Features

- Chat via OpenAI-compatible HTTP; fallback URL on request failure
- Prompt trimmed to ~6000 chars (~2048-token class)
- RAG: FTS5 keyword search over `slim/skills`, `slim/docs`, `slim/data`
- Tools (one JSON call then a final answer): `rag_search`, `read_file`, `write_file` (jailed, 8 KiB), `mqtt_publish` only if `SLIM_MQTT_HTTP` is set
- Skills: `slim/skills/*.md` injected as text; scripts are not executed
- Session turns stored in `slim/data/slim.sqlite`

## Tests

```bash
python3 -S slim/test_agent.py
python3 -S slim/test_rag.py
python3 -S slim/test_tools.py
```

## C++ executable (same features)

Host:

```bash
make -C slim/cpp host
./slim/cpp/slim-agent --help
```

T830 (OpenWrt musl gcc 9.3):

```bash
make -C slim/cpp t830
file slim/cpp/slim-agent-t830
# ELF aarch64, interpreter /lib/ld-musl-aarch64.so.1
# NEEDED: libstdc++.so.6 libgcc_s.so.1 libc.so
```

Copy `slim-agent-t830` plus `slim/skills` onto the CPE. Same flags as the Python client (`--base-url`, `--model`, `--once`, `--data-dir`). RAG is keyword scan of `.md`/`.txt` (no SQLite). Not hardware-verified on T830.

## Out of scope

Gateway, dashboard, Honcho, embeddings, MCP, arbitrary shell, full Hermes skills, 3B local models.
