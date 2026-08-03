# Pi2 Agent — Raspberry Pi 2+ AI Agent

<p align="center">
  <b>Lightweight AI Agent for ARMv7 / 1GB RAM devices</b>
</p>

This is a minimal AI agent optimized for **Raspberry Pi 2** (ARMv7 32-bit, 1GB RAM) and similar constrained Linux devices.

Primarily designed for embedded-system development: always-on controllers, sensor/automation nodes, lab devices, robotics gateways, home/industrial IoT boxes, and other constrained deployments where low memory use, predictable dependencies, and remote-first AI services matter more than desktop-heavy local stacks.

Raspberry Pi 2 is the baseline target, not the upper limit. The same profile is intended for Pi2-class or better Linux systems, including Raspberry Pi 3/4/5, Pi Zero 2 W, ARM64 SBCs, x86 mini PCs, and VMs. Higher-spec devices can keep the conservative Pi2 defaults or enable heavier features later.

## Repository

```text
https://github.com/Matt0080828/new_agent
```

Branch:

```text
main
```

## What is preserved

- CLI entrypoint
- core agent loop and provider routing
- tools system and toolsets
- skills system
- persistent memory and session search
- cron scheduler
- delegation/subagents
- MCP and ACP code paths
- gateway and platform adapters
- plugins and memory provider plugins
- OpenAI-compatible endpoints, including local `llama.cpp` servers

## What is slimmed

The Pi2 profile avoids eager install/use of heavy features:

- browser automation runtime
- local Chromium/Playwright-style stacks
- uvloop / `uvicorn[standard]`; dashboard/API use uvicorn's native Python asyncio mode
- image/video generation backends
- voice/STT dependencies such as faster-whisper
- TTS premium providers
- torch / sentence-transformers / chromadb by default

The CLI itself uses the standard Python (sync) shell backend by default.

## Quickstart (Raspberry Pi 2 / ARMv7 Linux)

One-command install:

```bash
curl -fsSL https://raw.githubusercontent.com/Matt0080828/new_agent/main/setup-hermes.sh | bash
```

Requirements: **Raspberry Pi 2 or better** (ARMv7 32-bit, 1GB RAM), running Raspberry Pi OS (Bookworm) or Debian 12 based distribution. Python 3.10+ and git must be installed.

After install, add your API key and start:

```bash
export AGENT_API_KEY="sk-..."
agent
```

The CLI will guide you through model selection on first run.

### Use with a local model

If you have `llama.cpp` or `lm-studio` serving an OpenAI-compatible endpoint:

```bash
agent --provider local --model llama.cpp
```

For 1GB RAM devices, use 3B-class models with Q4_K_M quantization.

## Documentation

- [RASPBERRY_PI2_MANUAL.md](RASPBERRY_PI2_MANUAL.md) — full setup guide for Pi2
- [README_PI2.md](README_PI2.md) — detailed Pi2 profile reference

## License

See [LICENSE](LICENSE).
