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
from rag import connect, ingest_tree, log_turn, search
from skills import format_skills, load_skills
from tools import TOOLS, parse_tool_call, run_tool

DEFAULT_MAX_TOKENS = 256
DEFAULT_TIMEOUT = 120
DEFAULT_HISTORY = 6
MAX_BODY = 256 * 1024
MAX_PROMPT_CHARS = 6000


def _die(msg, code=2):
    sys.stderr.write(msg + "\n")
    sys.exit(code)


def chat_completion(base_url, model, messages, api_key=None, max_tokens=DEFAULT_MAX_TOKENS, timeout=DEFAULT_TIMEOUT):
    base = base_url.rstrip("/")
    if base.endswith("/v1"):
        url = base + "/chat/completions"
    else:
        url = base + "/v1/chat/completions"
    payload = {
        "model": model,
        "messages": messages,
        "max_tokens": int(max_tokens),
        "stream": False,
    }
    data = json.dumps(payload).encode("utf-8")
    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json",
        "Content-Length": str(len(data)),
    }
    if api_key:
        headers["Authorization"] = "Bearer " + api_key
    req = urllib.request.Request(url, data=data, headers=headers, method="POST")
    raw = b""
    try:
        resp = urllib.request.urlopen(req, timeout=timeout)
        try:
            raw = resp.read(MAX_BODY + 1)
        finally:
            resp.close()
    except urllib.error.HTTPError as exc:
        body = exc.read(MAX_BODY)
        raise SlimError("HTTP %s from %s: %s" % (exc.code, url, body[:500].decode("utf-8", "replace")), 1)
    except urllib.error.URLError as exc:
        raise SlimError("request failed: %s" % exc.reason, 1)
    if len(raw) > MAX_BODY:
        raise SlimError("response larger than %s bytes" % MAX_BODY, 1)
    try:
        obj = json.loads(raw.decode("utf-8"))
    except ValueError as exc:
        raise SlimError("invalid JSON: %s" % exc, 1)
    choices = obj.get("choices") or []
    if not choices:
        raise SlimError("no choices in response", 1)
    message = choices[0].get("message") or {}
    content = message.get("content")
    if content is None:
        raise SlimError("empty assistant content", 1)
    return content


def complete_with_fallback(primary, fallback, model, fallback_model, messages, api_key, max_tokens, timeout):
    try:
        return chat_completion(primary, model, messages, api_key, max_tokens, timeout)
    except SlimError:
        if not fallback:
            raise
        return chat_completion(fallback, fallback_model or model, messages, api_key, max_tokens, timeout)


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
    skills = load_skills(cfg["skills_dir"])
    messages = [
        {"role": "system", "content": system_prompt(format_skills(skills))},
        {"role": "user", "content": user_text},
    ]
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
    )
    parsed = parse_tool_call(reply)
    if parsed:
        name, args = parsed
        try:
            result = run_tool(name, args, conn, cfg["data_dir"], cfg.get("mqtt_http") or "")
        except (ValueError, OSError) as exc:
            result = "tool error: %s" % exc
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
        )
    log_turn(conn, "user", user_text)
    log_turn(conn, "assistant", reply)
    return reply


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
    }
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
