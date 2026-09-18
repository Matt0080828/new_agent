# t830-slim-agent

The T830/FG370 CPE slim agent: a standalone project holding only the files this agent needs.
It was developed inside the Hermes fork at `Matt0080828/new_agent` and extracted from there
(`cc8a67c`) into a repository of its own with no shared history.

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

This repository is a fresh root: it shares no history with the Hermes fork it came from and
cannot affect it. The Pi2/IoT fork is a separate project again.
