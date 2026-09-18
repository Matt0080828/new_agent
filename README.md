# t830-slim-agent

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
slim/README.md   the manual: build, deploy, install, configure, and what was verified
```

## Build and test on the host

```bash
cd slim/cpp && make && make test                              # 74 + 54 + 39 + 43 = 210 checks
cd slim && python3 -m unittest discover -s . -p 'test_*.py'    # 89 tests, standard library only
```

## Build for the CPE

```bash
make -C slim/cpp t830           # dynamic, 121,736 bytes; needs libstdc++/libgcc on the device
make -C slim/cpp t830-static    # 698,664 bytes, NEEDED 0, stripped; nothing to install there
```

## Put it on the CPE

```bash
./slim/deploy/run-on-t830.sh    # container + adb, push, sha256 both sides, on-device smoke test
```

The device is reachable over **adb on USB** - its management IP answers ARP but every TCP port
is closed, `/dev/ttyACM*` are modem AT ports, and the RNDIS gadget hands out no DHCP. The
manual in `slim/README.md` covers the rest: "Deploy to the CPE", "Run it on the device", and
"Install it so it survives a reboot, and configure it" - the last one says where to put the
binary (`/data/slim/...`), every setting with its flag, environment variable and default, and
why `--data-dir` has to be passed explicitly.

## Verified on real hardware

| Check | Result on the CPE |
| --- | --- |
| artifact integrity | `sha256 5de6ea50...` matches on host and device (`NEEDED 0`, stripped) |
| `./slim/deploy/run-on-t830.sh` | exit 0: slash commands with read-back, the session store, `--dry-run-writes` writing nothing, every fail-closed refusal |
| live model turn (LAN server) | a streamed answer, then `--history 6` recalling the number from the earlier turn, then `--history 0` failing to - the negative control |
| session store | `sessions/live.jsonl`, mode `0600`, one JSON object per line |
| failing model (HTTP 400) | reports `HTTP status 400: <server message>` and writes no session file |

A rebuild of this tree reproduces that exact artifact, so compare the sha256 before deploying.
The full verified tables, the measured device facts, and the deploy scripts are in
`slim/README.md` and `slim/deploy/`.
