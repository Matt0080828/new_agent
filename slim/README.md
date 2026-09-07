# t830-slim agent

Stdlib-only OpenAI-compatible chat client. This branch is for a **T830 / FG370 OpenWrt** slim path. It does **not** install or run the full Hermes / `new_agent` Python package on the CPE.

Not hardware-verified on T830.

## Why a separate client

Full `new_agent` (`hermes-agent` 0.18.2) needs Python `>=3.11,<3.14`, pip, venv, and wheels with C/Rust extensions (`pydantic-core`, `cryptography`, `Pillow`, …). T830 is OpenWrt aarch64 musl, ~1.65 GiB RAM, with WiFi7/5G/mesh often leaving 400–600 MiB. That install contract does not match.

This client uses only the Python standard library.

## Run (any Linux with python3)

```bash
python3 -S slim/agent.py --help
python3 -S slim/agent.py \
  --base-url http://127.0.0.1:8080/v1 \
  --model <id-from-/v1/models> \
  --once 'ping'
```

Environment: `SLIM_BASE_URL`, `SLIM_MODEL`, `SLIM_API_KEY`, `SLIM_MAX_TOKENS` (default 256), `SLIM_TIMEOUT` (default 120).

Local llama.cpp on T830 should stay around 270M Q4 and ctx 2048. Do not load 3B models on the CPE.

## Tests

```bash
python3 -S slim/test_agent.py
```

## Scope

- Keep Hermes core on `main` (Pi2 / Debian). Do not delete `agent/`, `hermes_cli/`, tools, gateway.
- This directory is the T830 starting point: remote or local OpenAI-compatible HTTP, no pip.
- Do not claim OpenWrt/T830 success until a device run exists.
