# t830-slim-agent

The T830/FG370 CPE slim agent, extracted from `Matt0080828/new_agent` (branch `t830-slim`,
`cc8a67c`) into a standalone branch containing only the files this agent needs.

* `slim/` — the agent: a C++ client for the CPE (`slim/cpp/`, cross-compiled for aarch64 musl)
  and a Python client with the same behaviour for hosts that have Python.
* `slim/README.md` — how to build it, deploy it to the CPE, and what was verified on real hardware.

```bash
# host build + C++ tests (needs only a C++11 compiler)
cd slim/cpp && make && make test

# CPE cross-build (needs the FG370/T830 SDK toolchain)
cd slim/cpp && make t830 t830-static

# Python client tests (standard library only)
cd slim && python3 -m unittest discover -s . -p 'test_*.py'
```

This branch is a fresh root: it shares no history with the other branches and cannot affect them.
The Hermes fork (and the Pi2/IoT line) remain in `main` / `t830-slim`.

License: MIT — upstream Hermes copyright preserved, see `LICENSE`.
