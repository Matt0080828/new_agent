"""Streaming chat transport. Stdlib only.

The completion request is streamed so the operator sees tokens as they are produced,
which is the difference between "it hung" and "it is thinking" on a slow CPE.

Retry rule: retry is allowed only *before* the first delta reaches the caller. Once
text has been shown, a retry would silently restart generation behind the operator's
back, so a mid-stream failure is final and reported as an error.
"""
from __future__ import print_function

import json
import time
import urllib.error
import urllib.request

from error import SlimError
from retry import (DEFAULT_ATTEMPTS, DEFAULT_BASE_DELAY, DEFAULT_MAX_DELAY,
                   DEFAULT_MAX_TOTAL, is_retryable_exception,
                   retry_delay_seconds)
from sse import SseParser, delta_content

MAX_STREAM_BYTES = 256 * 1024
READ_SIZE = 4096


class StreamResult(object):
    """Accumulated reply plus how it went."""

    def __init__(self, text="", status=0, error="", deltas=0):
        self.text = text
        self.status = status
        self.error = error
        self.deltas = deltas


def _describe(exc):
    if isinstance(exc, urllib.error.HTTPError):
        try:
            body = exc.read(1024)
        except Exception:  # noqa: BLE001 - the body is best-effort
            body = b""
        return "HTTP %s: %s" % (exc.code, body[:300].decode("utf-8", "replace"))
    if isinstance(exc, urllib.error.URLError):
        return "request failed: %s" % getattr(exc, "reason", exc)
    return "%s: %s" % (type(exc).__name__, exc)


def read_stream(response, on_delta, max_bytes=MAX_STREAM_BYTES):
    """Read an event stream, calling on_delta for every content delta.

    Returns (text, delta_count, saw_done).
    """
    parser = SseParser()
    parts = []
    deltas = 0
    total = 0
    # read1() returns whatever has arrived, while read(n) on a buffered HTTP
    # response blocks until n bytes or EOF - which would defeat streaming entirely
    # and only print the whole answer at the end.
    reader = getattr(response, "read1", None) or response.read

    def drain():
        count = 0
        while True:
            payload = parser.next_payload()
            if payload is None:
                break
            text = delta_content(payload)
            if text:
                parts.append(text)
                count += 1
                if on_delta:
                    on_delta(text)
        return count

    while True:
        chunk = reader(READ_SIZE)
        if not chunk:
            break
        if isinstance(chunk, bytes):
            chunk = chunk.decode("utf-8", "replace")
        total += len(chunk)
        if total > max_bytes:
            raise ValueError("stream larger than %s bytes" % max_bytes)
        parser.feed(chunk)
        deltas += drain()
    parser.flush()
    deltas += drain()
    return "".join(parts), deltas, parser.done()


def stream_completion(url, payload, api_key=None, headers=None, timeout=120,
                      on_delta=None, retry_policy=None, log=None, max_bytes=MAX_STREAM_BYTES,
                      sleep=time.sleep, clock=time.monotonic, opener=None):
    """POST `payload` (already a dict with stream=true) and stream the answer back."""
    data = json.dumps(payload).encode("utf-8")
    head = {
        "Content-Type": "application/json",
        "Accept": "text/event-stream",
        "Content-Length": str(len(data)),
    }
    if headers:
        head.update(headers)
    if api_key:
        head["Authorization"] = "Bearer " + api_key

    policy = retry_policy or {}
    attempts = max(1, int(policy.get("attempts", DEFAULT_ATTEMPTS)))
    base_delay = policy.get("base_delay", DEFAULT_BASE_DELAY)
    max_delay = policy.get("max_delay", DEFAULT_MAX_DELAY)
    max_total = policy.get("max_total", DEFAULT_MAX_TOTAL)
    started = clock()
    last_error = "stream failed"
    # Injectable seam: tests pass a fake opener instead of patching the urllib module.
    open_url = opener or urllib.request.urlopen

    for i in range(1, attempts + 1):
        emitted = []

        def emit(text, _emitted=emitted):
            _emitted.append(text)
            if on_delta:
                on_delta(text)

        try:
            request = urllib.request.Request(url, data=data, headers=head, method="POST")
            response = open_url(request, timeout=timeout)
            try:
                text, deltas, saw_done = read_stream(response, emit, max_bytes)
            finally:
                try:
                    response.close()
                except Exception:  # noqa: BLE001
                    pass
        except Exception as exc:  # noqa: BLE001 - classification decides
            last_error = _describe(exc)
            # Fail closed on retry: once a delta was shown, a retry is not a retry of
            # the same user-visible operation, it is a silent restart.
            retryable = is_retryable_exception(exc) and not emitted
            if log:
                log("[slim] stream attempt %d/%d error=%s %s"
                    % (i, attempts, last_error,
                       "retryable" if retryable else "final (output already shown)" if emitted else "final"))
            if not retryable or i == attempts:
                raise SlimError(last_error, 1)
            delay = retry_delay_seconds(i, base_delay, max_delay)
            if max_total is not None and max_total > 0 and (clock() - started) + delay > max_total:
                raise SlimError(last_error + " (retry budget exhausted)", 1)
            sleep(delay)
        else:
            if log:
                log("[slim] stream attempt %d/%d ok deltas=%d%s"
                    % (i, attempts, deltas, "" if saw_done else " (no [DONE])"))
            error = "" if saw_done else "stream ended without [DONE]"
            return StreamResult(text=text, status=200, error=error, deltas=deltas)

    raise SlimError(last_error, 1)
