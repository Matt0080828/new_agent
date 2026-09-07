#!/usr/bin/env python3
"""Stdlib-only OpenAI-compatible chat client for constrained CPE / OpenWrt.

No third-party imports. Intended as a T830 / FG370 starting point, not a
full Hermes replacement. Not hardware-verified on T830.
"""
from __future__ import print_function

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

DEFAULT_MAX_TOKENS = 256
DEFAULT_TIMEOUT = 120
DEFAULT_HISTORY = 8
MAX_BODY = 256 * 1024


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
        _die("HTTP %s from %s: %s" % (exc.code, url, body[:500].decode("utf-8", "replace")), 1)
    except urllib.error.URLError as exc:
        _die("request failed: %s" % exc.reason, 1)
    if len(raw) > MAX_BODY:
        _die("response larger than %s bytes" % MAX_BODY, 1)
    try:
        obj = json.loads(raw.decode("utf-8"))
    except ValueError as exc:
        _die("invalid JSON: %s" % exc, 1)
    choices = obj.get("choices") or []
    if not choices:
        _die("no choices in response", 1)
    message = choices[0].get("message") or {}
    content = message.get("content")
    if content is None:
        _die("empty assistant content", 1)
    return content


def _parse_args(argv):
    p = argparse.ArgumentParser(
        description="Stdlib slim agent: OpenAI-compatible chat, no pip deps."
    )
    p.add_argument("--base-url", default=os.environ.get("SLIM_BASE_URL", ""), help="e.g. http://127.0.0.1:8080/v1")
    p.add_argument("--model", default=os.environ.get("SLIM_MODEL", ""), help="model id from /v1/models")
    p.add_argument("--api-key", default=os.environ.get("SLIM_API_KEY", ""), help="optional Bearer token")
    p.add_argument("--max-tokens", type=int, default=int(os.environ.get("SLIM_MAX_TOKENS", DEFAULT_MAX_TOKENS)))
    p.add_argument("--timeout", type=int, default=int(os.environ.get("SLIM_TIMEOUT", DEFAULT_TIMEOUT)))
    p.add_argument("--once", default="", help="single prompt then exit (non-interactive)")
    return p.parse_args(argv)


def main(argv=None):
    args = _parse_args(argv if argv is not None else sys.argv[1:])
    if not args.base_url:
        _die("set --base-url or SLIM_BASE_URL")
    if not args.model:
        _die("set --model or SLIM_MODEL")
    history = []
    if args.once:
        history.append({"role": "user", "content": args.once})
        print(chat_completion(args.base_url, args.model, history, args.api_key or None, args.max_tokens, args.timeout))
        return 0
    if not sys.stdin.isatty():
        prompt = sys.stdin.read()
        if not prompt.strip():
            _die("empty stdin")
        history.append({"role": "user", "content": prompt})
        print(chat_completion(args.base_url, args.model, history, args.api_key or None, args.max_tokens, args.timeout))
        return 0
    sys.stderr.write("slim agent. empty line or Ctrl-D to quit.\n")
    while True:
        try:
            line = input("> ")
        except EOFError:
            break
        if not line.strip():
            break
        history.append({"role": "user", "content": line})
        if len(history) > DEFAULT_HISTORY:
            history = history[-DEFAULT_HISTORY:]
        try:
            reply = chat_completion(args.base_url, args.model, history, args.api_key or None, args.max_tokens, args.timeout)
        except SystemExit:
            raise
        print(reply)
        history.append({"role": "assistant", "content": reply})
    return 0


if __name__ == "__main__":
    sys.exit(main())
