# T830 Agent — MediaTek MT7992 CLI-only AI Agent

<p align="center">
  <b>CLI-only agent for MediaTek T830 (MT7992, aarch64 OpenWrt-musl)</b>
</p>

This branch (`t830-strip-gateway`) is Hermes Agent stripped to a **CLI-only**
agent for the MediaTek **T830 / MT7992** (aarch64, NEON, ~1.65 GB RAM) running
OpenWrt-musl. It is the edge build of the same `new_agent` fork — the messaging
gateway, desktop apps, and web/dashboard surfaces are removed so the footprint
fits an always-on CPE box with a small on-box model.

The model runs against an **OpenAI-compatible endpoint** — either an on-box
`llama.cpp` server (FunctionGemma-270M) or a remote/LAN one. There is no local
browser, no dashboard, and no chat-platform integration in this build.

## Repository / branch

```text
repo:   https://github.com/Matt0080828/new_agent
branch: t830-strip-gateway
```

## What was removed (this branch)

- `apps/` — desktop app, bootstrap installer
- `tui_gateway/` — the TUI/desktop backend server
- `gateway/platforms/` — all messaging adapters (telegram/discord/slack/…)
- `plugins/platforms/` + `plugins/dashboard_auth/`
- `hermes_cli/` web_server / send_cmd / dashboard / gui / gateway / whatsapp / slack subcommands
- core deps `fastapi` / `uvicorn` / `python-multipart` and the `[web]` extra
- ~396 obsolete tests + 4 dead test dirs

Kept: the CLI entrypoint, core agent loop, tools/toolsets, skills, memory +
session search, cron, delegation/subagents, MCP/ACP, the surviving
`gateway.status` / `gateway.session_context` / `gateway.platforms.base` core,
and OpenAI-compatible provider routing. `hermes_cli/gateway.py` (process
management: `find_gateway_pids`, restart/stop) is kept because the CLI's
cron/status paths call it.

## Run on the T830 (on-box, via OpenWrt package)

The T830 is provisioned through a custom OpenWrt `opkg` package set (built
separately — see `T830_hermes_agent/` on the build host, which packages this
source as `hermes-agent` + `llama-cpp` + optional `python3.11`). This repo is
the *source* that the `hermes-agent` package's payload wheel is built from.

On-device layout after `opkg install hermes-agent`:

```text
/usr/lib/hermes-agent/venv       # created ON the T830 by install.sh (offline wheels)
/usr/lib/hermes-agent/bin/hermes # the CLI
/etc/hermes/                     # $HERMES_HOME (config.yaml, .env, sessions, memory)
```

The venv is created on the target with the target's own `python3.11`
(`pip install --no-index --find-links wheels/`) so the box needs **no network**
at install/upgrade time. Then:

```bash
# 1. point it at the model (on-box llama.cpp server, or remote)
echo 'OPENAI_BASE_URL=http://127.0.0.1:8080/v1' >> /etc/hermes/.env
echo 'OPENAI_API_KEY=***'      >> /etc/hermes/.env

# 2. run the agent (serial console or ssh)
/usr/lib/hermes-agent/bin/hermes
```

The bundled profile is `files/config.t830.yaml` (copied to
`$HERMES_HOME/config.yaml` on first start) — CLI-only, small context floor,
heavy toolsets disabled.

### On-box model (recommended): FunctionGemma-270M via llama.cpp

```text
llama-server (llama-cpp pkg)   :8080, 127.0.0.1
  └─ functiongemma-270m-it Q4_K_M  (~200 MB, English, 2K ctx, ~30-50 tok/s on A55)
       ↓ OpenAI-compatible /v1
hermes (hermes-agent pkg)      CLI @ serial/ssh
```

FunctionGemma-270M is a function-calling specialist — the right floor model for
tool use at this size (base-only 120–135 MB models have no reliable function
calling). It is trained at a **2K working context**, so keep the context floor
small:

```yaml
agent:
  minimum_tool_context_length: 2048   # 2K matches FunctionGemma's training window
```

A larger window costs KV-cache RAM and degrades quality on this model; the
default `MINIMUM_CONTEXT_LENGTH = 32_000` in the source is a cloud-model safety
margin that the profile overrides down for local inference.

### Remote / LAN model

For a stronger model (64K+ context, full tool use) run it on a LAN machine or
cloud and point the T830 at it:

```bash
echo 'OPENAI_BASE_URL=https://llama.lan.example:8080/v1' >> /etc/hermes/.env
echo 'OPENAI_API_KEY=***' >> /etc/hermes/.env
```

`hermes setup model` (or editing `config.yaml` `providers:`) also works for a
named OpenAI-compatible provider.

## Verify the CLI

```bash
hermes --version
hermes --help
```

The messaging/dashboard subcommands are gone in this build and correctly report
"invalid choice":

```bash
$ hermes gateway --help     # -> error: invalid choice (expected)
$ hermes dashboard --help   # -> error: invalid choice (expected)
```

## Develop / verify off-box (x86 or ARM dev host)

Plain editable install for testing on a dev machine (Python `>=3.11,<3.14`):

```bash
git clone -b t830-strip-gateway https://github.com/Matt0080828/new_agent.git
cd new_agent
python3 -m venv ~/.hermes-venv && source ~/.hermes-venv/bin/activate
pip install -e ".[cli,pty]"
hermes setup model
hermes
```

`pip install -e ".[cli,pty]"` pulls only the CLI surface — no web/messaging
extras. Use `hermes tools` / `hermes tools enable <name>` to re-enable any
optional toolset later; install the matching extra first if the tool reports a
missing dependency.

## Context & performance defaults

| Model | Context floor | Notes |
|---|---|---|
| FunctionGemma-270M (on-box) | 2048 | matches 2K training window; small KV-cache |
| LAN / cloud model | 32000+ (default) | full tool use, coding, shared RAG |

`MINIMUM_CONTEXT_LENGTH` (32K in source) is the *default* floor; the
`minimum_tool_context_length` config value overrides it per deployment. Both
paths only warn when a local server reports a context below the active floor —
they do not hard-reject the session.

## Memory & RAG posture

- built-in agent memory + session search (SQLite/FTS local state) — on by default
- optional RAG: keep embeddings **remote** on the T830; do not run a local
  torch / sentence-transformers / chromadb stack in 1.65 GB RAM

## RAM budget (T830, ~1.65 GB measured)

| Component | Approx |
|---|---|
| system idle | ~480 MB |
| FunctionGemma-270M Q4_K_M (llama-server) | ~300 MB |
| hermes venv + agent | ~300 MB |
| peak headroom | tight — swap may engage at WiFi peaks |

To stay comfortable: run `llama-server` on-demand (not resident), use
`Q3_K_M` (~11 MB smaller) if needed, and put `$HERMES_HOME` on USB/eMMC.

## License

See [LICENSE](LICENSE).
