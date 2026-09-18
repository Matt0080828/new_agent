# t830-slim agent

Stdlib rewrite: chat + SQLite FTS RAG + whitelist tools + markdown skills.
Not Hermes. Not hardware-verified on T830.

## Run

```bash
python3 -S slim/agent.py --help
python3 -S slim/agent.py --ingest-only
python3 -S slim/agent.py --base-url http://127.0.0.1:8080/v1 --model tiny --once 'what is this device'
```

Env: `SLIM_BASE_URL`, `SLIM_FALLBACK_URL`, `SLIM_MODEL`, `SLIM_FALLBACK_MODEL`, `SLIM_API_KEY`, `SLIM_MAX_TOKENS`, `SLIM_TIMEOUT`, `SLIM_DATA_DIR`, `SLIM_SKILLS_DIR`, `SLIM_DOCS_DIR`, `SLIM_MQTT_HTTP`, `SLIM_ALLOW_WRITE`, `SLIM_ALLOW_MQTT`, `SLIM_ATTEMPTS`, `SLIM_RETRY_BASE_MS`, `SLIM_RETRY_MAX_TOTAL_S`, `SLIM_HTTP_VERBOSE`, `SLIM_STREAM`, `SLIM_SESSION`, `SLIM_HISTORY`, `SLIM_CONFIG`.

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
- Session turns stored in `slim/data/slim.sqlite` (protected by the policy above)

## Tests

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
make -C slim/cpp test        # tests/test_slim: 70 checks, tests/test_sse: 45 checks
./slim/cpp/slim-agent --help
```

T830 (OpenWrt musl gcc 9.3):

```bash
make -C slim/cpp t830          # dynamic, stripped, 117 KB
make -C slim/cpp t830-static   # self-contained, 694 KB, no NEEDED libs
file slim/cpp/slim-agent-t830-static
# ELF aarch64, statically linked, stripped, interpreter-less
```

Two artifacts on purpose. `slim-agent-t830` is the small one and needs
`libstdc++.so.6`, `libgcc_s.so.1` and `libc.so` on the device - OpenWrt images usually
have no C++ runtime, and checking for it requires logging into the device.
`slim-agent-t830-static` links musl and libstdc++ in, so it needs nothing, which is the
safer default when the device cannot be inspected first. Both are stripped: the unstripped
build carried debug info and an `RPATH` pointing at the build machine's SDK path.

`-lgcc_eh` must come after the source files on the static link, otherwise the link fails
with `undefined reference to _Unwind_Resume`.

Copy `slim-agent-t830-static` (or `slim-agent-t830`) plus `slim/skills` onto the CPE. Flags:
`--base-url`, `--model`, `--once`, `--data-dir`, `--max-tokens`, the retry flags
`--attempts`, `--retry-base-ms`, `--http-verbose`, `--stream`, `--dry-run-writes`, and the
policy flags `--allow-write`, `--allow-mqtt`, `--non-interactive` / `--interactive`. The
270M-class local model does
not emit usable tool JSON, so tools are reached through the `/rag`, `/read`, `/write`,
`/mqtt`, `/help` slash commands rather than model tool calls; RAG is a keyword scan of
`.md`/`.txt` (no SQLite in the C++ client). Not hardware-verified on T830.

## Out of scope

Gateway, dashboard, Honcho, embeddings, MCP, arbitrary shell, full Hermes skills, 3B local models.
