# Board compatibility

Which development boards the slim agent runs on - and what is actually the
constraint.

## The agent is not the constraint

The agent never runs a model. It is a thin OpenAI-compatible client (chat
completions plus SSE) with a local keyword-RAG and tool layer, and its
footprint is:

| Artifact | Size | Dependencies |
| --- | --- | --- |
| C++ client, static (aarch64 musl) | 719,144 bytes | none (`NEEDED 0`, stripped) |
| C++ client, dynamic (aarch64 musl) | 142,352 bytes | musl loader + libstdc++ on the device |
| C++ client, host (`g++ -O2 -std=c++11`) | same sources | glibc (or the platform C++ runtime) |
| Python client | sources | Python 3, standard library only (no pip packages) |

No npm, no virtualenv, no service manager. A board that can execute a 1 MB
static ELF - or that already has Python 3 - can run the agent; its resident
memory is a few MB. The decision that matters is **where the model server
lives**.

## Mode A - the model on the same board (llama.cpp)

Then the board's RAM has to hold weights + KV cache + the OS. Rules of thumb
for Q4 models:

| Model class (Q4) | Weights | Board RAM needed | Example boards |
| --- | --- | --- | --- |
| 270M-0.5B | ~200-470 MB | ~1 GB | the reference CPE (**verified**), RPi 3B+ / RPi 4 (1-2 GB), Orange Pi Zero 2W (2 GB), Rock Pi S (2 GB), MT7986/MT7988-class CPE |
| 1-1.5B | ~0.6-1 GB | ~2-4 GB | RPi 4 (4 GB), RPi 5, Orange Pi 5, Khadas VIM3, Rock 5B |
| 3B and up | 2 GB+ | 8 GB+, or a separate accelerator | x86 mini-PC (N100 class) and up - or use Mode B |

The anchor numbers come from the verified CPE: 1,736,840 kB RAM (1,183,552 kB
available) ran `Qwen2.5-0.5B-Instruct-Q4_K_M` (469 MB file) with the server at
646 MB RSS, `-c 2048 -t 4`, ~4 s per turn. The manual's conclusion: on that
box, "much past 1B will not fit". A 270M-class model needs roughly 200 MB of
weights, which is why 1 GB-class boards work at all. The KV cache adds tens
of MB at `-c 2048` for these small models and grows with context; the agent
trims its prompt to ~6000 chars (~2048-token class) precisely so a 2048-slot
server context is enough.

## Mode B - the model on the LAN or in the cloud

Then the board only runs the agent and a 1 MB static binary plus a few MB of
RAM is the entire footprint: RPi Zero 2W and other 512 MB-class boards, 32-bit
SBCs, x86 NAS boxes, Docker containers, a router with a small flash partition.
Point `--base-url` at any OpenAI-compatible server (llama.cpp, LM Studio,
vLLM, an API key) and optionally `--fallback-url` for a local-first-with-LAN-
escape setup. This direction is verified too: the reference CPE answered through an
LM Studio on the host PC across the CPE's LAN (45-63 s per turn on the shared
endpoint).

## Verified configurations

- **The reference CPE** - OpenWrt
  23.05.5, aarch64 musl, **no python3 in the image**, `/tmp` tmpfs 838 MB,
  `/overlay` 116 MB, `/data` 12.5 GB. Static binary sha256-matched from the
  host; both modes exercised on the box (see `slim/README.md` for the
  transcripts).
- **x86_64 host** - full test suite plus smoke runs; the C++ build is plain
  `g++ -O2 -std=c++11` with POSIX sockets and nothing else.

## Architectures

| Architecture | Status | Notes |
| --- | --- | --- |
| aarch64 | **verified** | the reference CPE; any ARM64 Linux is in the same class |
| x86_64 | **verified** | host build and tests |
| armv7 (32-bit) | untested, expected to build | the sources are plain POSIX C++11 with no architecture-specific code; bring an armv7 toolchain (`CXX=... make`) - RPi 1/2 and MT7621-class routers are the usual targets |
| mips / RISC-V | untested | same expectation, no guarantee |
| non-Linux | macOS works as a host; Windows and ESP32-class targets are not supported | POSIX sockets, `sys/stat.h`, and a real Python 3 for the Python client |

## Bringing it up on a new board

1. **Get the agent binary onto the board.**
   - ARM64 OpenWrt: `make -C slim/cpp t830-static` (the `TC` variable points
     at the reference OpenWrt toolchain; any aarch64 musl gcc will do) and
     `slim/deploy/run-on-t830.sh` is the pattern: push, compare sha256 on
     both sides, smoke test.
   - Any other Linux: `CXX=<board-cross-g++> make -C slim/cpp host`, or
     deploy the Python client if the board has Python 3.
2. **Verify the bare agent first**: `--help`, one slash-command turn
   (`/rag`, `/read`), one `/history` - none of which need a model.
3. **Add the model** per the Mode A table (llama.cpp static for the target
   architecture), or point at an existing endpoint for Mode B.
4. **Put the data directory on persistent storage** (on the CPE: `/data`):
   sessions, the command allowlist and the RAG corpus live there, and `/tmp`
   is tmpfs on the reference device.

## Caveats

- All RAM figures are Q4-family. FP16 and Q8 multiply them by 2-4.
- A 270M-0.5B model is weak and does not emit the tool JSON format
   reliably; the slash commands remain the deterministic way to drive
   actions, and the tool-call path is a bonus when a bigger model answers.
- The `t830-static` artifact is aarch64 **static musl**: it runs on any
  aarch64 Linux (musl or glibc, e.g. RPi OS) with nothing installed. The
  dynamic `t830` build instead needs musl and libstdc++ on the device.
  For other architectures, build the host target with the board's compiler,
  or use the Python client.
