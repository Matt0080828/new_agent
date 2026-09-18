# t830-slim agent

Stdlib rewrite: chat + SQLite FTS RAG + whitelist tools + markdown skills.
Not Hermes.

The C++ client in `cpp/` is **verified on a real T830 CPE** (OpenWrt 23.05.5, aarch64 musl,
`evb6990_cpe_mt7990_emmc`, no python3 in the image): pushed over adb, sha256-matched, and
run on the device - slash commands with read-back, the session store, the change-queue dry
run and every fail-closed refusal behaved, with `real 0m 0.00s` startup. Live model turns are
verified there as well: a streamed answer from a model server on the LAN, the session file it
records, history replay that recalled a number from an earlier turn, and `--history 0` as the
negative control - "Deploy to the CPE" has the commands and the full table.

This is a standalone project (not a Hermes or IoT fork): clone it and build it with

```bash
git clone https://github.com/Matt0080828/t830-slim-agent.git && cd t830-slim-agent/slim/cpp
make && make test        # host build + the four C++ test binaries
```

## Run

```bash
python3 -S slim/agent.py --help
python3 -S slim/agent.py --ingest-only
python3 -S slim/agent.py --base-url http://127.0.0.1:8080/v1 --model tiny --once 'what is this device'
```

Env: `SLIM_BASE_URL`, `SLIM_FALLBACK_URL`, `SLIM_MODEL`, `SLIM_FALLBACK_MODEL`, `SLIM_API_KEY`, `SLIM_MAX_TOKENS`, `SLIM_TIMEOUT`, `SLIM_DATA_DIR`, `SLIM_SKILLS_DIR`, `SLIM_DOCS_DIR`, `SLIM_MQTT_HTTP`, `SLIM_ALLOW_WRITE`, `SLIM_ALLOW_MQTT`, `SLIM_DB`, `SLIM_DRY_RUN_WRITES`, `SLIM_ATTEMPTS`, `SLIM_RETRY_BASE_MS`, `SLIM_RETRY_MAX_TOTAL_S`, `SLIM_HTTP_VERBOSE`, `SLIM_STREAM`, `SLIM_SESSION`, `SLIM_HISTORY`, `SLIM_CONFIG`.

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

## Retries (bounded, and never for mutations)

The transport retries a request only when repeating it cannot change the outcome
in a harmful way:

- **Retried:** transport failures (no response), `429`, and `5xx`. A completion
  request has no lasting server-side effect, so another attempt is safe.
- **Not retried:** any other `4xx`, a config error (a non-`http://` URL, an
  oversized response, a peer that is not speaking HTTP), and any exception type
  the policy does not recognise — unknown means "do not retry".
- **Never retried: `mqtt_publish`.** A publish is a mutation, and a retry after an
  ambiguous failure can duplicate the effect. It is sent exactly once; the Python
  test suite serves a `500` and asserts the server saw one request.
- Bounded: `--attempts` (default 3), `--retry-base-ms` (default 500, doubled per
  attempt with a 4 s cap) and an overall budget (`SLIM_RETRY_MAX_TOTAL_S`, default
  20 s) so a dead endpoint cannot hold a turn open indefinitely.
- `--http-verbose` (or `SLIM_HTTP_VERBOSE=1`) logs every attempt:

```
[slim] http attempt 1/3 error=URLError: <urlopen error [Errno 111] Connection refused> retryable
[slim] http attempt 3/3 error=URLError: <urlopen error [Errno 111] Connection refused> retryable
```

Each provider in the fallback chain gets its own retry budget, so a fallback after
an unreachable primary is still attempted under the same policy.

## Streaming (`--stream`)

Both clients can read the reply as server-sent events, so tokens appear while the
model is still generating instead of after it has finished:

```bash
python3 -S slim/agent.py --stream --once 'status'
./slim/cpp/slim-agent --stream --once 'status'
```

- `--stream` (or `SLIM_STREAM=1`) is off by default, because it changes how a turn is
  printed: deltas go straight to stdout and the assembled reply is not printed again.
- The parser is incremental (`slim/sse.py`, `slim/cpp/sse.cpp`). A socket chunk
  boundary may fall inside a line, inside the JSON, or between the two newlines that
  end an event, and a stream that ends without a trailing newline is still parsed -
  the tests feed one byte at a time to cover every split point.
- **Retry stops once a delta has been shown.** Before the first delta a retry is safe;
  after it, another attempt would restart generation behind the operator's back, so a
  mid-stream failure is final and reported on stderr. The C++ test proves the retry is
  not attempted by making the server offer a second response that must never be
  requested.
- A stream that ends without `[DONE]` keeps the text and reports
  `stream ended without [DONE]`.
- On the Python side the reader uses `read1()`: `read(n)` on a buffered HTTP response
  blocks until `n` bytes or EOF, which silently turns streaming back into one dump at
  the end.
- If a server answers with an event stream even though `stream` was not requested, the
  non-streaming path assembles the data lines instead of returning only the first delta.

## Sessions

A session is an append-only JSONL file, one turn per line:

```json
{"ts":"2026-09-18T04:09:45Z","role":"user","text":"溫度 25.5°C 是什麼"}
```

```bash
python3 -S slim/agent.py --session boiler --history 8 --once 'status'
python3 -S slim/agent.py --session boiler --show-history
./slim/cpp/slim-agent --session boiler --history 8 --once 'status'
./slim/cpp/slim-agent --session boiler --once '/history'
```

- Stored under `<data-dir>/sessions/<name>.jsonl`, mode `0600`. The name is validated
  before it becomes a file name (`..`, `/`, a leading dot and more than 64 characters
  are refused), so it cannot be used to leave the data directory.
- Both clients write the same bytes, so a session started with one can be continued
  with the other, and non-ASCII text is stored raw (never `\u`-escaped) so either
  reader gets the characters back.
- `--history N` replays the last N turns into the prompt before the new question, which
  is what makes a session resumable rather than a log nobody reads. `0` replays
  nothing, `-1` replays everything; the default is 6.
- **The store is not writable through a tool.** `write_file` refuses any path with a
  `sessions` component, in both clients, even with `--allow-write`: history the model can
  rewrite is not history.
- Reading is tolerant: a truncated or hand-edited line is skipped, because a broken tail
  must not lock the operator out of their own history. A failed *write* is reported on
  stderr and does not kill the turn - losing history is bad, losing the answer that was
  asked for is worse.
- The same JSON decode handles `\uXXXX` (and surrogate pairs) as UTF-8, so a server that
  escapes non-ASCII (Python's `json.dumps` does by default) does not turn `溫度` into
  `u6eabu5ea6`.

## Tool output budget and the change queue

Two bounds that exist because the smallest client has no room for what the largest one
tolerates:

- **Tool output is budgeted.** Every successful tool result passes one `ToolBudget` (8 KiB
  per turn by default). Over the budget it is trimmed with an explicit
  `[truncated N bytes: tool output budget reached]` marker, and once the budget is gone
  later results come back as `[tool output omitted: ...]` - so the model learns its tool
  output was cut instead of silently receiving nothing. Errors are never budgeted: they
  are short, and they are what the model needs to see.
- **File changes are queued, not written as they are asked for.** A model-initiated
  `write_file` is staged, and the queue is applied once when the turn ends, with one audit
  line per file:

```
[slim] wrote notes.md (412 bytes)
[slim] changes: 2 file(s), 900 bytes, 1 overwrite(s) of an earlier staged change
```

  Last write to the same path wins and that is reported; the queue is capped (16 files,
  64 KiB) and so is each file (8 KiB), so a loop cannot fill the device in one turn. A
  failure while applying one file does not discard the others. Protected paths
  (`sessions/`, `*.sqlite`, dotfiles) are refused at staging time, not at apply time.

The C++ client prints the budget summary and the manifest per turn too, and its
`--dry-run-writes` flag stages and reports without writing anything at all.

## Features

- Chat via OpenAI-compatible HTTP; fallback URL on request failure
- Prompt trimmed to ~6000 chars (~2048-token class)
- RAG: FTS5 keyword search over `slim/skills`, `slim/docs`, `slim/data`
- Tools (one JSON call then a final answer): `rag_search`, `read_file`, `write_file` (jailed, 8 KiB), `mqtt_publish` only if `SLIM_MQTT_HTTP` is set
- Skills: `slim/skills/*.md` injected as text; scripts are not executed
- Session turns stored as append-only JSONL under `<data-dir>/sessions/`; the RAG corpus
  is `<data-dir>/slim.sqlite` (SQLite FTS5, never writable through a tool)

## Tests

All seven files together are **89 tests**; the one-shot form is
`python3 -S -m unittest discover -s slim -p 'test_*.py'`.

```bash
python3 -S slim/test_agent.py
python3 -S slim/test_rag.py
python3 -S slim/test_tools.py
python3 -S slim/test_policy.py   # policy + the state-file regression
python3 -S slim/test_retry.py    # retry policy + "mqtt is never retried"
python3 -S slim/test_sse.py      # SSE parser, streaming and "no retry after a delta"
python3 -S slim/test_session.py  # session store, format compatibility with the C++ client
```

`test_policy.py` includes the regression that matters most: open the default-layout
database, attempt `write_file slim.sqlite`, and assert the database still reads back.
`test_sse.py` covers the SSE parser at every chunk split, the incremental-delivery
ordering guarantee, and a real loopback socket. `test_session.py` pins the line format
both clients must agree on, the tolerant reader, and the `\uXXXX` decode.

## C++ executable (same features)

Host:

```bash
make -C slim/cpp host
make -C slim/cpp test        # 74 + 54 + 39 + 43 = 210 checks in four binaries
./slim/cpp/slim-agent --help
```

T830 (OpenWrt musl gcc 9.3):

```bash
make -C slim/cpp t830          # dynamic, stripped, 121,736 bytes (~119 KB)
make -C slim/cpp t830-static   # self-contained, 698,664 bytes (~682 KB), no NEEDED libs
file slim/cpp/slim-agent-t830-static
# ELF aarch64, statically linked, stripped, interpreter-less
```

The current tree builds to `sha256 5de6ea50…` (static) - the same bytes that were run on
the device, so a rebuild can be compared with the verified build before deploying.

Two artifacts on purpose. `slim-agent-t830` is the small one and needs
`libstdc++.so.6`, `libgcc_s.so.1` and `libc.so` on the device - OpenWrt images usually
have no C++ runtime, and checking for it requires logging into the device.
`slim-agent-t830-static` links musl and libstdc++ in, so it needs nothing, which is the
safer default when the device cannot be inspected first. Both are stripped: the unstripped
build carried debug info and an `RPATH` pointing at the build machine's SDK path.

`-lgcc_eh` must come after the source files on the static link, otherwise the link fails
with `undefined reference to _Unwind_Resume`.

## Deploy to the CPE, and what was verified there

Everything below assumes a clone of <https://github.com/Matt0080828/t830-slim-agent>
(this README is that repository's `slim/README.md`).

The C++ client is the only client that can run on the T830: the image ships **no python3**,
so the Python agent is not a smaller option, it is no option.

The reachable entry point is **adb over USB**, not the network: the management IP answers
ARP but every TCP port is closed, `/dev/ttyACM*` are modem AT ports, and the RNDIS gadget
does not hand out DHCP. On a host whose user is in the `docker` group, adb runs inside a
privileged container, because the USB node is root-only:

```bash
docker run -d --name adb-t830-run --privileged -v /dev/bus/usb:/dev/bus/usb \
  debian:bookworm-slim sleep infinity
docker exec adb-t830-run bash -c 'apt-get update -qq && apt-get install -y -qq adb'
docker exec adb-t830-run adb devices -l      # 0123456789ABCDEF  device  usb:1-7
```

`adb push` executes inside that container, so it cannot read host paths: `docker cp` the
payload in first, push from inside, and **compare sha256 on both sides before running
anything**. Both scripts are in this repository (`slim/deploy/`), so a clone is enough:

```bash
make -C slim/cpp t830-static          # the artifact the script pushes
./slim/deploy/run-on-t830.sh          # container + adb + push + sha256 + on-device smoke test
./slim/deploy/run-on-t830.sh --no-smoke   # just get the binary onto the device
```

### Run it on the device

By hand, if you would rather see every step (the container from above is running):

```bash
mkdir -p /tmp/stage && cp slim/cpp/slim-agent-t830-static /tmp/stage/
docker cp /tmp/stage/slim-agent-t830-static adb-t830-run:/tmp/stage-binary
docker exec adb-t830-run adb shell "mkdir -p /tmp/slim"
docker exec adb-t830-run adb push /tmp/stage-binary /tmp/slim/slim-agent-t830-static
docker exec adb-t830-run adb shell "chmod +x /tmp/slim/slim-agent-t830-static"
sha256sum /tmp/stage/slim-agent-t830-static                                     # host side
docker exec adb-t830-run adb shell sha256sum /tmp/slim/slim-agent-t830-static    # device side
```

The `docker cp` in the middle is not decoration: `adb` runs inside the container, so
`docker exec … adb push <host path>` cannot see the file and fails with a bare
"push failed" - the trap the paragraph above warns about.

Then, with no model server at all - this is what `on-device-smoke.sh` automates, and it needs
no network, writes only under its own `/tmp/slim-smoke/data-$$`, and deletes nothing:

```bash
docker exec adb-t830-run adb shell 'cd /tmp/slim && ./slim-agent-t830-static --data-dir /tmp/slim/data \
  --session smoke --once "/help"'
docker exec adb-t830-run adb shell 'cd /tmp/slim && ./slim-agent-t830-static --data-dir /tmp/slim/data \
  --session smoke --once "/write note.txt hello-from-t830"'
docker exec adb-t830-run adb shell 'cd /tmp/slim && cat /tmp/slim/data/note.txt'   # read it back
docker exec adb-t830-run adb shell 'cd /tmp/slim && ./slim-agent-t830-static --data-dir /tmp/slim/data \
  --dry-run-writes --session smoke --once "/write nope.txt should-not-exist"'
```

A live turn, with a model server on the LAN (`<host>` = the machine running LM Studio, on the
CPE's own `br-lan` segment - the verified run used `192.168.1.210:1234`). The path below is
where `run-on-t830.sh` leaves the binary (`/tmp/slim-smoke/`); a hand push puts it in
`/tmp/slim/`.

```bash
docker exec adb-t830-run adb shell 'cd /tmp/slim-smoke && ./slim-agent-t830-static --data-dir /tmp/slim-live \
  --session live --non-interactive \
  --base-url http://<host>:1234/v1 --model qwen2.5-7b-instruct-1m --stream --once "Remember the number 7391"'
docker exec adb-t830-run adb shell 'cd /tmp/slim-smoke && ./slim-agent-t830-static --data-dir /tmp/slim-live \
  --session live --history 6 --non-interactive \
  --base-url http://<host>:1234/v1 --model qwen2.5-7b-instruct-1m --once "What number did I ask you to remember?"'
```

That second command is the point of `--history`: it answers `7391` because the first turn was
replayed; add `--history 0` and it cannot. Keep the whole run under `/tmp` (the image is
read-only apart from `/overlay` and `/data`). A turn through a shared LM Studio takes 45-63 s
cold, including the model load, and about a second once the model is resident - the C++ client
has no timeout flag, its socket timeout is compiled in, see below. Clean up with
`rm -rf /tmp/slim /tmp/slim-smoke` (whichever you created) when you are done - nothing outside
those directories is touched.

Run on the device, this is the transcript, not a summary:

```
$ ./slim-agent-t830-static --data-dir /tmp/slim-live --session live --stream --once "Remember the number 7391..."
The number is 7391.
$ ... --history 6 --once "What number did I ask you to remember?"
You asked me to remember the number 7391.
$ ... --history 0 --once "What number did I ask you to remember?"
I don't remember specific numbers or details from previous conversations once the session ends.
```

Those three turns left `live.jsonl` (mode `0600`) holding six lines - `user`, `assistant`,
three times over - in the JSONL format above.

### Install it so it survives a reboot, and configure it

`/tmp` is tmpfs, so the copy `run-on-t830.sh` pushes is gone after a reboot. Installing means
putting the binary on a persistent filesystem and giving the agent a writable data directory.
Measured on the device: `/overlay` (`/dev/loop0`, ext4) has 116 MB free and `/data`
(`/dev/block/user_data`, ext4) 12.5 GB, and neither is mounted `noexec` - the binary runs from
either (verified: `--help` from `/data/slim` exits 0).

```bash
docker cp slim/cpp/slim-agent-t830-static adb-t830-run:/tmp/stage-binary
docker exec adb-t830-run adb shell 'mkdir -p /data/slim/skills /data/slim/docs /data/slim/data &&
  cp /tmp/stage-binary /data/slim/slim-agent && chmod 755 /data/slim/slim-agent'
docker exec adb-t830-run adb shell 'sha256sum /data/slim/slim-agent'    # still 5de6ea50...
docker exec adb-t830-run adb shell 'cd /data/slim && ./slim-agent --help | head -3'
```

That install is 700 KB. Keep the corpus and the sessions on `/data` (the roomier one) and the
binary wherever you like.

The C++ client has **no config file**: flags and environment only, and **a flag beats the
environment** (verified on the device - `SLIM_SESSION=envsess` plus `--session flagwins`
created `flagwins.jsonl`). Every setting it understands, with the real defaults from
`agent.cpp`:

| Setting | Flag | Environment | Default |
| --- | --- | --- | --- |
| Model endpoint | `--base-url` | `SLIM_BASE_URL` | empty - a model turn needs it |
| Fallback endpoint | `--fallback-url` | `SLIM_FALLBACK_URL` | empty |
| Model id | `--model` | `SLIM_MODEL` | empty |
| Fallback model | `--fallback-model` | `SLIM_FALLBACK_MODEL` | empty |
| API key | `--api-key` | `SLIM_API_KEY` | empty (LM Studio wants none) |
| Data dir: sessions, scratch | `--data-dir` | `SLIM_DATA_DIR` | `./slim/data`, **relative to the working directory** |
| Markdown skills | `--skills-dir` | `SLIM_SKILLS_DIR` | `./slim/skills` |
| RAG corpus | `--docs-dir` | `SLIM_DOCS_DIR` | `./slim/docs` |
| MQTT bridge | `--mqtt-http` | `SLIM_MQTT_HTTP` | empty - `/mqtt` stays unavailable |
| Session name | `--session` | `SLIM_SESSION` | `default` |
| Replayed turns | `--history` | `SLIM_HISTORY` | `6` (`0` none, negative every turn) |
| Reply cap | `--max-tokens` | `SLIM_MAX_TOKENS` | `64` |
| Streaming | `--stream` | `SLIM_STREAM=1` | off |
| Model may write files | `--allow-write` | `SLIM_ALLOW_WRITE=1` | off, denied |
| Model may publish | `--allow-mqtt` | `SLIM_ALLOW_MQTT=1` | off, denied |
| Approval prompt | `--non-interactive` / `--interactive` | - | on only when stdin is a tty, otherwise deny |
| Stage writes, write nothing | `--dry-run-writes` | `SLIM_DRY_RUN_WRITES=1` | off |
| Retry budget | `--attempts`, `--retry-base-ms` | `SLIM_ATTEMPTS`, `SLIM_RETRY_BASE_MS` | `3`, `500` ms |
| Per-attempt log | `--http-verbose` | `SLIM_HTTP_VERBOSE=1` | off |
| Socket timeout | - | - | compiled in, `120` s |

(The Python client reads a few more - `SLIM_DB`, `SLIM_CONFIG`, `SLIM_TIMEOUT`,
`SLIM_RETRY_MAX_TOTAL_S` - which the C++ build does not know.)

Those relative defaults are the trap worth knowing: run the binary from the read-only root and
`./slim/data` cannot be created, so turns are answered but never recorded. Always pass
`--data-dir` (as above), and `--skills-dir` / `--docs-dir` when the corpus lives elsewhere.
On the device a wrapper beats a long command line:

```bash
cat > /data/slim/run <<'EOF'
#!/bin/sh
# All configuration lives here; anything passed on the command line still wins.
BASE=http://192.168.1.210:1234/v1        # the machine serving the model
MODEL=qwen2.5-7b-instruct-1m
DIR=/data/slim
exec "$DIR/slim-agent" \
  --data-dir "$DIR/data" --skills-dir "$DIR/skills" --docs-dir "$DIR/docs" \
  --base-url "$BASE" --model "$MODEL" --non-interactive --history 6 "$@"
EOF
chmod +x /data/slim/run
/data/slim/run --once 'what is this device'
/data/slim/run --once '/rag openwrt'       # search the corpus you put in docs/
```

Adding `--allow-write` to that call is the only way the model may write, and even then never to
the session store, any `*.sqlite`, or outside the data directory. The corpus is just files:
`--skills-dir` and `--docs-dir` are scanned for `.md`/`.txt`, and `/rag` searches them.

```bash
# same staging rule as the binary: docker cp into the container, adb push onto the device
docker cp ./cpe-notes.md adb-t830-run:/tmp/cpe-notes.md
docker exec adb-t830-run adb push /tmp/cpe-notes.md /data/slim/docs/cpe-notes.md
docker exec adb-t830-run adb shell '/data/slim/run --once "/rag openwrt"'
# [{"path":"doc/openwrt.md","snippet":"vice runs OpenWrt 23.05.5 with a musl aarch64 userland..."}]
```

A hit names its source (`doc/...` for `--docs-dir`, `skill/...` for `--skills-dir`, `data/...`
for the data directory itself), so an empty corpus answers with an empty list rather than an
error. To update, push the new
binary over `/data/slim/slim-agent` after comparing its sha256, and leave the data directory
alone - that is where the sessions are.

Verified on the device (static build, sha256-matched):

| Check | Result on the CPE |
| --- | --- |
| `--help` | full flag list, `real 0m 0.00s` |
| `/write note.txt ...` then read it back | file written, content read back |
| `/history` | reports an empty session until a model turn is recorded |
| `--dry-run-writes` | prints the manifest and writes nothing |
| `sessions/...`, `*.sqlite`, `../escape` writes | all refused, with the expected wording |
| unreachable model endpoint | 3 attempts, then fails: bounded retry, no loop |

Device facts measured on the box: OpenWrt 23.05.5, kernel 5.15.167,
`MediaTek evb6990_cpe_mt7990_emmc`, 1,736,840 kB RAM (1,183,552 kB available), `/tmp` tmpfs
838 MB, `/overlay` 116 MB free, `/data` 12.5 GB, root shell, and both
`/lib/ld-musl-aarch64.so.1` and `libstdc++.so.6.0.30` present (so the dynamic artifact runs
too, but the static one needs nothing).

Flags accepted by the C++ client (its `--help` prints a shorter summary): `--base-url`,
`--fallback-url`, `--model`, `--fallback-model`, `--api-key`, `--data-dir`, `--skills-dir`,
`--docs-dir`, `--mqtt-http`, `--max-tokens`, `--attempts`, `--retry-base-ms`,
`--http-verbose`, `--stream`, `--dry-run-writes`, `--session`, `--history`, `--allow-write`,
`--allow-mqtt`, `--non-interactive` / `--interactive`, `--once`, `--ingest-only`, `--help`.
The Python client's `--help` lists its full set, which is the same plus `--config`, `--db`,
`--timeout`, `--show-history`. The 270M-class local model does not
emit usable tool JSON, so tools are reached through the `/rag`, `/read`, `/write`, `/mqtt`,
`/history`, `/help` slash commands rather than model tool calls; RAG is a keyword scan of
`.md`/`.txt` (no SQLite in the C++ client).

**Verified on the device: a live model turn.** With the host PC on the CPE's LAN
(`br-lan 192.168.1.0/24`), LM Studio on the host is reachable from the CPE and both builds
answer through it:

| Check | Result on the CPE |
| --- | --- |
| streamed turn (`--stream`, static build) | `qwen2.5-7b-instruct-1m` answered; first turn 54-63 s including the model load |
| session record | `sessions/live.jsonl`, mode 600, one JSON object per line |
| history replay (`--history 6`, dynamic build) | a later turn answered `7391` - the number the first turn was asked to remember |
| `--history 0` | the same question gets no answer from history (`42` in one run, "I don't remember..." in another), so replay really is off |
| failing model (the server answers 400) | reports `HTTP status 400: <server message>` and writes no session file |

The earlier "no route to the model server" note is resolved by putting the host on the CPE's
LAN. The `rndis0` gadget remains an option, but OpenWrt's netifd removes manually added
addresses, so it has to be configured through `uci` or inside the same shell invocation.

Turn latency with a shared LM Studio is 45-63 s. The C++ client has no timeout flag - its
socket timeout is compiled in (`cfg.timeout = 120` in `agent.cpp`), which is enough for those
turns but not unlimited, so a slow server fails the turn instead of hanging. If you need a
longer one, change that constant and rebuild; the Python client has the knob as
`--timeout` / `SLIM_TIMEOUT`. Exceeding it cuts the turn mid-prefill, and LM Studio logs
`Client disconnected. Stopping generation...`.

## Out of scope

Gateway, dashboard, Honcho, embeddings, MCP, arbitrary shell, full Hermes skills, 3B local models.
