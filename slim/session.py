"""Append-only session store. Stdlib only.

One JSON object per line:

    {"ts":"2026-09-18T12:00:00Z","role":"user","text":"..."}

The format is deliberately the same as the C++ client's (slim/cpp/session.cpp), so the
same session file works with either client on the CPE, and so a session can be read
with grep when something looks wrong.

Reading is tolerant: a truncated or hand-edited line is skipped instead of failing the
whole session, because a broken tail must not lock the operator out of their history.
"""
from __future__ import print_function

import datetime
import json
import os
import re

MAX_LINE_BYTES = 16 * 1024       # one turn of a 2048-token model
MAX_FILE_BYTES = 1024 * 1024     # history is read back in full
_NAME_RE = re.compile(r"^[A-Za-z0-9._-]{1,64}$")


def valid_session_name(name):
    """(ok, reason). A session name becomes a file name, so validate before use."""
    if not name:
        return False, "session name is empty"
    if not _NAME_RE.match(name):
        return False, ("session name may only contain letters, digits, '-', '_' and '.' "
                       "(max 64 characters)")
    if name.startswith("."):
        return False, "session name must not start with a dot"
    if ".." in name:
        return False, "session name must not contain '..'"
    return True, ""


def session_path(data_dir, name):
    return os.path.join(data_dir.rstrip("/") or "/", "sessions", name + ".jsonl")


def _now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def session_append(path, role, text):
    """Append one turn (creating the directory). Returns (ok, reason)."""
    parent = os.path.dirname(path)
    if parent and not os.path.isdir(parent):
        try:
            os.makedirs(parent)
        except OSError as exc:
            return False, "cannot create %s: %s" % (parent, exc)
    # Compact separators and raw UTF-8 so a line written here is byte-compatible with
    # the C++ writer (which does not escape non-ASCII either).
    line = json.dumps({"ts": _now(), "role": role, "text": text},
                      separators=(",", ":"), ensure_ascii=False) + "\n"
    try:
        # Append-only and owner-only, matching the C++ writer.
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
        try:
            os.write(fd, line.encode("utf-8"))
        finally:
            os.close(fd)
    except OSError as exc:
        return False, "cannot append to %s: %s" % (path, exc)
    return True, ""


def session_load(path, limit=-1):
    """The last `limit` turns, oldest first.

    limit < 0 (the default) returns every turn, 0 returns none. A missing file means an
    empty session.
    """
    if not os.path.isfile(path):
        return []
    turns = []
    try:
        with open(path, "rb") as fh:
            raw = fh.read(MAX_FILE_BYTES + 1)
    except OSError:
        return []
    for line in raw.split(b"\n"):
        if not line or len(line) > MAX_LINE_BYTES:
            continue
        try:
            obj = json.loads(line.decode("utf-8", "replace"))
        except ValueError:
            continue  # tolerate a truncated or hand-edited line
        if not isinstance(obj, dict):
            continue
        role, text = obj.get("role"), obj.get("text")
        if not isinstance(role, str) or not isinstance(text, str):
            continue
        turns.append({"ts": obj.get("ts") or "", "role": role, "text": text})
    if limit >= 0:
        turns = turns[-limit:] if limit > 0 else []
    return turns
