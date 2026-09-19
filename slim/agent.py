#!/usr/bin/env python3
"""Stdlib slim agent: chat + FTS RAG + whitelist tools + markdown skills.

Not a Hermes subset. Not hardware-verified on T830.
"""
from __future__ import print_function

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from error import SlimError
from policy import approve_tool, load_allowlist
from retry import DEFAULT_ATTEMPTS, DEFAULT_BASE_DELAY, DEFAULT_MAX_TOTAL, run_with_retry
from rag import connect, ingest_tree, log_turn, search
from session import session_append, session_load, session_path, valid_session_name
from skills import format_skills, load_skills
from sse import assemble
from stream import stream_completion
from tools import TOOLS, ChangeQueue, ToolBudget, parse_tool_call, run_tool

DEFAULT_MAX_TOKENS = 256
DEFAULT_TIMEOUT = 120
DEFAULT_HISTORY = 6
MAX_BODY = 256 * 1024
MAX_PROMPT_CHARS = 6000


def _die(msg, code=2):
    sys.stderr.write(msg + "\n")
    sys.exit(code)


def _retry_log(line):
    """Audit line for each transport attempt (stderr, one per line)."""
    sys.stderr.write(line + "\n")


def _record_turn(cfg, role, text):
    """Append one turn to the session store. A failing store is reported but never
    kills the turn: losing history is bad, losing the answer that was asked for is
    worse."""
    path = cfg.get("session_file")
    if not path:
        return
    ok, why = session_append(path, role, text)
    if not ok:
        sys.stderr.write("[slim] session: %s\n" % why)


def _apply_changes(queue, budget):
    """Write what the turn staged and report it. This is the operator's audit line: a model
    that asked for five writes must be visible as five lines, not as silence."""
    if budget.used or budget.elided:
        sys.stderr.write("[slim] %s\n" % budget.summary())
    if not queue.entries:
        return
    wrote, failures = queue.apply()
    for line in wrote:
        sys.stderr.write("[slim] %s\n" % line)
    for line in failures:
        sys.stderr.write("[slim] change failed: %s\n" % line)
    sys.stderr.write("[slim] changes: %s\n" % queue.summary())


def _stream_to_stdout():
    """Write each delta straight to stdout, unbuffered: that is the point of
    streaming on a slow CPE."""
    def emit(text):
        sys.stdout.write(text)
        sys.stdout.flush()
    return emit


def _post_once(url, data, headers, timeout):
    """One HTTP attempt. Raises the transport exception unchanged so the retry
    policy can classify it, rather than wrapping it in SlimError first."""
    req = urllib.request.Request(url, data=data, headers=headers, method="POST")
    resp = urllib.request.urlopen(req, timeout=timeout)
    try:
        return resp.read(MAX_BODY + 1)
    finally:
        resp.close()


def chat_completion(base_url, model, messages, api_key=None, max_tokens=DEFAULT_MAX_TOKENS,
                    timeout=DEFAULT_TIMEOUT, retry_policy=None, log=None, stream=False,
                    on_delta=None):
    base = base_url.rstrip("/")
    if base.endswith("/v1"):
        url = base + "/chat/completions"
    else:
        url = base + "/v1/chat/completions"
    payload = {
        "model": model,
        "messages": messages,
        "max_tokens": int(max_tokens),
        "stream": bool(stream),
    }
    data = json.dumps(payload).encode("utf-8")
    headers = {
        "Content-Type": "application/json",
        "Accept": "text/event-stream" if stream else "application/json",
        "Content-Length": str(len(data)),
    }
    if api_key:
        headers["Authorization"] = "Bearer " + api_key
    if stream:
        # Deltas reach on_delta as they arrive; the assembled text comes back here.
        result = stream_completion(url, payload, api_key=api_key, headers=headers,
                                   timeout=timeout, on_delta=on_delta,
                                   retry_policy=retry_policy, log=log)
        if result.error:
            sys.stderr.write("[slim] stream: %s\n" % result.error)
        if not result.text:
            raise SlimError(result.error or "empty stream", 1)
        return result.text
    # Retried: a completion request has no lasting server-side effect, so repeating
    # it after a transport error, a 429 or a 5xx is safe. Mutations are never
    # retried (see the mqtt_publish branch of tools.run_tool).
    policy = retry_policy or {}
    try:
        raw = run_with_retry(
            lambda: _post_once(url, data, headers, timeout),
            attempts=policy.get("attempts", DEFAULT_ATTEMPTS),
            base_delay=policy.get("base_delay", DEFAULT_BASE_DELAY),
            max_total=policy.get("max_total", DEFAULT_MAX_TOTAL),
            log=log,
        )
    except urllib.error.HTTPError as exc:
        try:
            body = exc.read(MAX_BODY)
        except Exception:
            body = b""
        raise SlimError("HTTP %s from %s: %s" % (exc.code, url, body[:500].decode("utf-8", "replace")), 1)
    except urllib.error.URLError as exc:
        raise SlimError("request failed: %s" % exc.reason, 1)
    if len(raw) > MAX_BODY:
        raise SlimError("response larger than %s bytes" % MAX_BODY, 1)
    try:
        obj = json.loads(raw.decode("utf-8"))
    except ValueError as exc:
        # A server may answer with an event stream even when stream was not
        # requested; assembling the data lines beats failing on the JSON parse
        # (and returns the whole reply, not just its first delta).
        text = assemble(raw.decode("utf-8", "replace"))
        if text:
            return text
        raise SlimError("invalid JSON: %s" % exc, 1)
    choices = obj.get("choices") or []
    if not choices:
        raise SlimError("no choices in response", 1)
    message = choices[0].get("message") or {}
    content = message.get("content")
    if content is None:
        raise SlimError("empty assistant content", 1)
    return content


def complete_with_fallback(primary, fallback, model, fallback_model, messages, api_key, max_tokens,
                           timeout, retry_policy=None, log=None, stream=False, on_delta=None):
    # Each provider gets its own retry budget, so a fallback after an unreachable
    # primary is still attempted with the same transport policy.
    try:
        return chat_completion(primary, model, messages, api_key, max_tokens, timeout,
                               retry_policy=retry_policy, log=log, stream=stream,
                               on_delta=on_delta)
    except SlimError:
        if not fallback:
            raise
        return chat_completion(fallback, fallback_model or model, messages, api_key, max_tokens,
                               timeout, retry_policy=retry_policy, log=log, stream=stream,
                               on_delta=on_delta)


def _trim(messages):
    out = list(messages)
    while out and sum(len(m.get("content") or "") for m in out) > MAX_PROMPT_CHARS:
        # keep system (index 0) if present
        if len(out) > 2 and out[0].get("role") == "system":
            out.pop(1)
        elif len(out) > 1:
            out.pop(0)
        else:
            break
    return out


def system_prompt(skill_text):
    lines = [
        "You are a small CPE agent. Answer briefly.",
        "Context window is about 2048 tokens. Do not claim Hermes compatibility.",
        "Tools (emit ONE JSON object, nothing else, if you need a tool):",
    ]
    lines.extend("- " + t for t in TOOLS)
    lines.append("After a tool result, answer in plain text. Never execute shell.")
    if skill_text:
        lines.append(skill_text)
    return "\n".join(lines)


def run_turn(user_text, cfg, conn):
    rlog = _retry_log if cfg.get("http_verbose") else None
    streaming = bool(cfg.get("stream"))
    emit = _stream_to_stdout() if streaming else None
    skills = load_skills(cfg["skills_dir"])
    messages = [{"role": "system", "content": system_prompt(format_skills(skills))}]
    # Replay the stored session first: that is what makes it resumable rather than a
    # log nobody reads.
    for turn in session_load(cfg.get("session_file", ""), cfg.get("history", 0)):
        messages.append({"role": "assistant" if turn["role"] == "assistant" else "user",
                         "content": turn["text"]})
    messages.append({"role": "user", "content": user_text})
    hits = search(conn, user_text, limit=3)
    if hits:
        blob = json.dumps(hits, ensure_ascii=False)
        messages.insert(1, {"role": "system", "content": "RAG hits: " + blob})
    messages = _trim(messages)
    reply = complete_with_fallback(
        cfg["base_url"],
        cfg.get("fallback_url") or "",
        cfg["model"],
        cfg.get("fallback_model") or "",
        messages,
        cfg.get("api_key") or None,
        cfg["max_tokens"],
        cfg["timeout"],
        retry_policy=cfg.get("retry") or {},
        log=rlog,
        stream=streaming,
        on_delta=emit,
    )
    parsed = parse_tool_call(reply)
    budget = ToolBudget()
    queue = ChangeQueue()
    if parsed:
        name, args = parsed
        detail = args.get("path") or args.get("topic") or ""
        ok, reason = approve_tool(cfg.get("policy") or {}, name, detail, human=False)
        sys.stderr.write("[slim] tool %s (%s) -> %s: %s\n"
                         % (name, detail, "allow" if ok else "deny", reason))
        if ok:
            try:
                result = run_tool(name, args, conn, cfg["data_dir"], cfg.get("mqtt_http") or "",
                                  queue=queue, budget=budget,
                                  exec_allow=(cfg.get("policy") or {}).get("exec_allow"),
                                  exec_path=cfg.get("exec_path"))
            except (ValueError, OSError) as exc:
                result = "tool error: %s" % exc
        else:
            result = "tool error: %s" % reason
        messages.append({"role": "assistant", "content": reply})
        messages.append({"role": "user", "content": "tool %s result:\n%s" % (name, result)})
        messages = _trim(messages)
        reply = complete_with_fallback(
            cfg["base_url"],
            cfg.get("fallback_url") or "",
            cfg["model"],
            cfg.get("fallback_model") or "",
            messages,
            cfg.get("api_key") or None,
            cfg["max_tokens"],
            cfg["timeout"],
            retry_policy=cfg.get("retry") or {},
            log=rlog,
            stream=streaming,
            on_delta=emit,
        )
    _apply_changes(queue, budget)
    log_turn(conn, "user", user_text)
    log_turn(conn, "assistant", reply)
    _record_turn(cfg, "user", user_text)
    _record_turn(cfg, "assistant", reply)
    # With streaming the deltas were already written to stdout, so the caller must
    # not print the text a second time.
    return "" if streaming else reply


def _load_config(path):
    cfg = {}
    if path and os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as fh:
            cfg = json.load(fh)
    return cfg


def _parse_args(argv):
    p = argparse.ArgumentParser(description="Stdlib slim agent: chat, FTS RAG, tools, skills.")
    p.add_argument("--config", default=os.environ.get("SLIM_CONFIG", ""), help="JSON config path")
    p.add_argument("--base-url", default=os.environ.get("SLIM_BASE_URL", ""))
    p.add_argument("--fallback-url", default=os.environ.get("SLIM_FALLBACK_URL", ""))
    p.add_argument("--model", default=os.environ.get("SLIM_MODEL", ""))
    p.add_argument("--fallback-model", default=os.environ.get("SLIM_FALLBACK_MODEL", ""))
    p.add_argument("--api-key", default=os.environ.get("SLIM_API_KEY", ""))
    p.add_argument("--max-tokens", type=int, default=int(os.environ.get("SLIM_MAX_TOKENS", DEFAULT_MAX_TOKENS)))
    p.add_argument("--timeout", type=int, default=int(os.environ.get("SLIM_TIMEOUT", DEFAULT_TIMEOUT)))
    p.add_argument("--data-dir", default=os.environ.get("SLIM_DATA_DIR", os.path.join(HERE, "data")))
    p.add_argument("--skills-dir", default=os.environ.get("SLIM_SKILLS_DIR", os.path.join(HERE, "skills")))
    p.add_argument("--docs-dir", default=os.environ.get("SLIM_DOCS_DIR", os.path.join(HERE, "docs")))
    p.add_argument("--db", default=os.environ.get("SLIM_DB", ""))
    p.add_argument("--mqtt-http", default=os.environ.get("SLIM_MQTT_HTTP", ""))
    p.add_argument("--allow-write", action="store_true",
                   default=os.environ.get("SLIM_ALLOW_WRITE", "") == "1",
                   help="allow model-initiated write_file (default: denied)")
    p.add_argument("--allow-exec", action="store_true",
                   help="let the model run commands (only bare names in <data-dir>/commands.allow)")
    p.add_argument("--allow-mqtt", action="store_true",
                   default=os.environ.get("SLIM_ALLOW_MQTT", "") == "1",
                   help="allow model-initiated mqtt_publish (default: denied)")
    p.add_argument("--non-interactive", action="store_true",
                   help="never prompt for approval (deny by default)")
    p.add_argument("--attempts", type=int, default=int(os.environ.get("SLIM_ATTEMPTS", 3)),
                   help="transport attempts per request (default 3)")
    p.add_argument("--retry-base-ms", type=int, default=int(os.environ.get("SLIM_RETRY_BASE_MS", 500)),
                   help="base backoff in ms, doubled per retry (default 500)")
    p.add_argument("--http-verbose", action="store_true",
                   default=os.environ.get("SLIM_HTTP_VERBOSE", "") == "1",
                   help="log every transport attempt to stderr")
    p.add_argument("--stream", action="store_true",
                   default=os.environ.get("SLIM_STREAM", "") == "1",
                   help="print tokens as they arrive (SSE); retry stops once output was shown")
    p.add_argument("--session", default=os.environ.get("SLIM_SESSION", "default"),
                   help="session name; turns go to <data-dir>/sessions/<name>.jsonl")
    p.add_argument("--history", type=int, default=int(os.environ.get("SLIM_HISTORY", 6)),
                   help="how many stored turns to replay into the prompt (default 6)")
    p.add_argument("--show-history", action="store_true",
                   help="print the stored turns for this session and exit")
    p.add_argument("--once", default="", help="single prompt then exit")
    p.add_argument("--ingest-only", action="store_true")
    return p.parse_args(argv)


def build_cfg(args):
    file_cfg = _load_config(args.config)
    def pick(key, argval):
        if argval:
            return argval
        return file_cfg.get(key) or ""

    data_dir = pick("data_dir", args.data_dir)
    cfg = {
        "base_url": pick("base_url", args.base_url),
        "fallback_url": pick("fallback_url", args.fallback_url),
        "model": pick("model", args.model),
        "fallback_model": pick("fallback_model", args.fallback_model),
        "api_key": pick("api_key", args.api_key),
        "max_tokens": args.max_tokens,
        "timeout": args.timeout,
        "data_dir": data_dir,
        "skills_dir": pick("skills_dir", args.skills_dir),
        "docs_dir": pick("docs_dir", args.docs_dir),
        "mqtt_http": pick("mqtt_http", args.mqtt_http),
        "db": args.db or os.path.join(data_dir, "slim.sqlite"),
        "policy": {
            "allow_write": bool(args.allow_write or file_cfg.get("allow_write")),
            "allow_mqtt": bool(args.allow_mqtt or file_cfg.get("allow_mqtt")),
            "allow_exec": bool(args.allow_exec or file_cfg.get("allow_exec")),
            # Read once, from the data directory. protected_path() keeps a tool from
            # writing it, so the model cannot grant itself a command; no file means
            # nothing may run.
            "exec_allow": load_allowlist(os.path.join(data_dir, "commands.allow")),
            # Only prompt when a terminal is attached and the operator did not
            # opt out; otherwise the gate denies instead of blocking forever.
            "interactive": (not args.non_interactive) and sys.stdin.isatty(),
        },
        "http_verbose": bool(args.http_verbose or file_cfg.get("http_verbose")),
        "stream": bool(args.stream or file_cfg.get("stream")),
        "session": str(args.session or file_cfg.get("session") or "default"),
        "history": max(0, int(args.history)),
        "retry": {
            "attempts": max(1, int(args.attempts)),
            "base_delay": max(0.0, args.retry_base_ms / 1000.0),
            "max_total": float(os.environ.get("SLIM_RETRY_MAX_TOTAL_S", 20.0)),
        },
    }
    # The session name becomes a file name: validate before it is used, fail closed.
    ok, why = valid_session_name(cfg["session"])
    if not ok:
        _die("bad --session: %s" % why)
    cfg["session_file"] = session_path(data_dir, cfg["session"])
    return cfg


def main(argv=None):
    args = _parse_args(argv if argv is not None else sys.argv[1:])
    cfg = build_cfg(args)
    if not os.path.isdir(cfg["data_dir"]):
        os.makedirs(cfg["data_dir"])
    conn = connect(cfg["db"])
    ingest_tree(conn, cfg["skills_dir"], prefix="skill")
    ingest_tree(conn, cfg["docs_dir"], prefix="doc")
    ingest_tree(conn, cfg["data_dir"], prefix="data")
    if args.ingest_only:
        print("ingested into %s" % cfg["db"])
        return 0
    if args.show_history:
        turns = session_load(cfg["session_file"], cfg["history"])
        if not turns:
            print("no turns recorded yet in %s" % cfg["session_file"])
        for turn in turns:
            print("%s  %s: %s" % (turn["ts"], turn["role"], turn["text"]))
        return 0
    if not cfg["base_url"]:
        _die("set --base-url or SLIM_BASE_URL")
    if not cfg["model"]:
        _die("set --model or SLIM_MODEL")
    if args.once:
        try:
            print(run_turn(args.once, cfg, conn))
        except SlimError as exc:
            _die(str(exc), exc.code)
        return 0
    if not sys.stdin.isatty():
        prompt = sys.stdin.read()
        if not prompt.strip():
            _die("empty stdin")
        try:
            print(run_turn(prompt, cfg, conn))
        except SlimError as exc:
            _die(str(exc), exc.code)
        return 0
    sys.stderr.write("slim agent (RAG/tools/skills). empty line or Ctrl-D to quit.\n")
    while True:
        try:
            line = input("> ")
        except EOFError:
            break
        if not line.strip():
            break
        try:
            print(run_turn(line, cfg, conn))
        except SlimError as exc:
            sys.stderr.write(str(exc) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
