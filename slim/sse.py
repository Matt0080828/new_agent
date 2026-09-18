"""Incremental server-sent-events parsing. Stdlib only.

A socket chunk boundary can fall anywhere - inside a line, inside an event, in the
middle of the JSON - so the parser keeps a buffer instead of assuming one chunk is
one line or one event. Anything that does not start with "data:" (comments, the
event:/id:/retry: fields) is ignored, per the SSE spec.

`assemble()` exists because a server may answer with an event stream even when
stream was not requested; a plain "first content" extraction would then return only
the first delta.
"""
from __future__ import print_function

import json

DONE_MARKER = "[DONE]"


class SseParser(object):
    """Feed it raw bytes as text; pull complete data payloads out."""

    def __init__(self):
        self._buf = ""
        self._pending = []
        self._ready = []
        self._done = False

    def feed(self, chunk):
        if not chunk:
            return
        self._buf += chunk
        while True:
            idx = self._buf.find("\n")
            if idx < 0:
                break
            line = self._buf[:idx]
            self._buf = self._buf[idx + 1:]
            self._handle_line(line[:-1] if line.endswith("\r") else line)

    def flush(self):
        """End of stream: a server may omit the trailing newline / blank line.

        The leftover buffer is processed as the last line first, otherwise it would
        sit in the buffer and corrupt whatever is parsed next.
        """
        if self._buf:
            line, self._buf = self._buf, ""
            self._handle_line(line[:-1] if line.endswith("\r") else line)
        if self._pending:
            self._ready.append("\n".join(self._pending))
            self._pending = []

    def next_payload(self):
        """One complete data payload, or None when nothing is ready."""
        if not self._ready:
            return None
        return self._ready.pop(0)

    def done(self):
        """True once the server sent "[DONE]"."""
        return self._done

    def _handle_line(self, line):
        if line == "":
            if self._pending:
                self._ready.append("\n".join(self._pending))
                self._pending = []
            return
        if not line.startswith("data:"):
            return
        value = line[5:]
        if value.startswith(" "):
            value = value[1:]
        if value == DONE_MARKER:
            self._done = True
            return
        self._pending.append(value)


def delta_content(payload):
    """The assistant text in one payload, or None.

    choices[0].delta.content for a streaming chunk; choices[0].message.content if a
    server answered in one shot inside the stream.
    """
    if not payload:
        return None
    try:
        obj = json.loads(payload)
    except ValueError:
        return None
    choices = obj.get("choices") or []
    if not choices:
        return None
    first = choices[0] or {}
    for key in ("delta", "message"):
        part = first.get(key) or {}
        content = part.get("content")
        if content:
            return content
    return None


def assemble(raw_body):
    """Assemble a whole event-stream body (already in memory) into the reply text."""
    if not raw_body:
        return ""
    parser = SseParser()
    parser.feed(raw_body)
    parser.flush()
    parts = []
    while True:
        payload = parser.next_payload()
        if payload is None:
            break
        text = delta_content(payload)
        if text:
            parts.append(text)
    return "".join(parts)
