# t830-slim-agent

**English** | [繁體中文](README.zh-TW.md) | [简体中文](README.zh-CN.md) | [日本語](README.ja.md) | [한국어](README.ko.md) | [Español](README.es.md)

A small agent that runs on the Askey T830 / Fibocom FG370 CPE (OpenWrt 23.05.5, aarch64 musl,
**no python3 in the image**): chat over an OpenAI-compatible endpoint, keyword RAG over
markdown, a whitelist tool layer, markdown skills - and a fail-closed policy in front of
anything that writes.

Two clients, one behaviour: `slim/cpp/` runs on the CPE (cross-compiled for aarch64 musl, with
a static artifact that needs nothing installed), and `slim/` holds a Python client with the
same features for hosts that have Python. This project is standalone - not Hermes, not the
Hermes IoT/Pi2 fork. It was developed inside the Hermes fork at `Matt0080828/new_agent` and
extracted from there (`cc8a67c`) into a repository of its own with no shared history.

```text
slim/            Python client: policy, session store, tools, RAG, tests
slim/cpp/        C++ client with the same features, plus four test binaries
slim/deploy/     run-on-t830.sh and on-device-smoke.sh - the deployment path
slim/README.md   the manual: build, deploy, install, configure, model on the device
```

## Build and test on the host

```bash
cd slim/cpp && make && make test                              # 81 + 54 + 39 + 53 = 227 checks
cd slim && python3 -m unittest discover -s . -p 'test_*.py'    # 97 tests, standard library only
```

## Build for the CPE

```bash
make -C slim/cpp t830           # dynamic, 138,240 bytes; needs libstdc++/libgcc on the device
make -C slim/cpp t830-static    # 715,048 bytes, NEEDED 0, stripped; nothing to install there
```

## Put it on the CPE

```bash
./slim/deploy/run-on-t830.sh    # container + adb, push, sha256 both sides, on-device smoke test
```

The device is reachable over **adb on USB** - its management IP answers ARP but every TCP port
is closed, `/dev/ttyACM*` are modem AT ports, and the RNDIS gadget hands out no DHCP. `adb`
runs inside a privileged container because the USB node is root-only, and a container cannot
read host paths, so those scripts stage every payload with `docker cp` before pushing.

## Install it, and how it is configured

`/tmp` is tmpfs, so the pushed copy is gone after a reboot. Install onto `/data` (12.5 GB free)
or `/overlay` (116 MB); neither is mounted `noexec`. The layout the scripts and the manual
assume is `/data/slim/{slim-agent,run,data,skills,docs}` - 700 KB plus whatever corpus you add.
The C++ client has **no config file**: flags and environment only, and **a flag beats the
environment**. Its defaults are relative to the working directory (`--data-dir` defaults to
`./slim/data`), which on the read-only root means turns are answered but never recorded - so
always pass `--data-dir` explicitly. The docs and skills directories follow the same
CWD-relative rule (`./slim/docs`, `./slim/skills`): with the layout above, run from `/data`
(or set `SLIM_DOCS_DIR` / `SLIM_SKILLS_DIR`) or `/rag` finds nothing. The full
flag/environment/default table, the install commands and a wrapper script that holds the
configuration are in `slim/README.md`.

## Using a model that runs on the T830 itself

The agent only knows an OpenAI-compatible endpoint, so a `llama.cpp` server on the device is a
`--base-url` change and nothing else - no LAN, no other setting:

```bash
# device: start the server on loopback (no service entry; on demand, stop with kill)
cd /data/slim && nohup ./llama-server -m models/qwen2.5-0.5b-instruct-q4_k_m.gguf \
  --host 127.0.0.1 --port 8080 -c 2048 -t 4 > llama-server.log 2>&1 &

# agent against it
./slim-agent --data-dir /data/slim/data --session local --non-interactive \
  --model qwen2.5-0.5b-instruct --base-url http://127.0.0.1:8080/v1 --stream --once "Reply with one word: pong"
```

What fits: the CPE has 1.7 GB RAM, and `Qwen2.5-0.5B-Instruct-Q4_K_M` (469 MB, pushed in 47 s)
loaded with 646 MB RSS and ~1.1 GB still free - much past 1B will not fit. The cross-built
server needs only `libstdc++.so.6`, `libgcc_s.so.1` and musl `libc`, all present in the image.
With `-t 4` a short turn takes ~4 s and the server reports 12 tok/s prompt eval and 7.9 tok/s
generation. A model this size is not the 7B you can serve from the LAN: expect weaker answers. It can
emit the tool JSON format (the prompt shows it), but less reliably, so the slash commands
(`/rag`, `/read`, `/write`, `/mqtt`, `/history`) stay the deterministic way to drive actions.
Both can coexist per invocation, and `--fallback-url` gives
local-first with a LAN escape hatch. `slim/README.md` has the staging commands, the measured
numbers, and the caveats.

## Verified on real hardware

| Check | Result on the CPE |
| --- | --- |
| artifact integrity | `sha256 1c2c17b3...` matches on host and device (`NEEDED 0`, stripped) |
| `./slim/deploy/run-on-t830.sh` | exit 0: slash commands with read-back, the session store, `--dry-run-writes` writing nothing, every fail-closed refusal |
| live model turn over the LAN | a streamed answer, then `--history 6` recalling the number from the earlier turn, then `--history 0` failing to - the negative control |
| **model on the device itself** | `llama-server` on loopback + a 0.5B Q4: `PONG!` in 4 s, 12 tok/s prompt / 7.9 tok/s generation, `/history` replaying those turns, and `--fallback-url` answering through a dead primary |
| session store | `sessions/<name>.jsonl`, mode `0600`, one JSON object per line |
| RAG corpus, no rebuild | markdown dropped into `docs/` and `skills/` is searched live: `/rag mt753x`, the multi-word `/rag probe -22`, `/rag HotSpotFlag` and `/rag OWE` each returned the right file first (2026-09-22) |
| failing model (HTTP 400) | reports `HTTP status 400: <server message>` and writes no session file |

A rebuild of this tree reproduces that exact artifact, so compare the sha256 before deploying.
The full verified tables, the measured device facts, and the deploy scripts are in
`slim/README.md` and `slim/deploy/`.
