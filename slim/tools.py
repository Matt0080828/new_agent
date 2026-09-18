"""Whitelist tools. Paths stay under data_dir. No shell.

Writes additionally go through the policy module: agent state (databases and
similar) is never writable, whoever asks, because one write_file call used to be
able to overwrite the agent's own database inside data_dir.
"""
import json
import os
import urllib.error
import urllib.request

from policy import protected_path
from rag import search as rag_search

MAX_WRITE = 8 * 1024
MAX_READ = 16 * 1024
MAX_TOOL_OUTPUT = 8 * 1024
MAX_QUEUED_FILES = 16
MAX_QUEUED_BYTES = 64 * 1024


class ToolBudget(object):
    """Bounds what tool output may enter the conversation.

    Without a bound, two or three tool rounds push the prompt far past the context window
    of a small local model, and the failure looks like the model ignoring its tools.
    """

    def __init__(self, max_total=MAX_TOOL_OUTPUT):
        self.max_total = max_total
        self.used = 0
        self.elided = 0

    def add(self, text):
        """The text, trimmed to what is left, with a marker when it had to trim."""
        if self.used >= self.max_total:
            self.elided += len(text)
            return "[tool output omitted: %d-byte budget exhausted]" % self.max_total
        left = self.max_total - self.used
        if len(text) <= left:
            self.used += len(text)
            return text
        self.elided += len(text) - left
        self.used += left
        return text[:left] + "\n[truncated %d bytes: tool output budget reached]" % (len(text) - left)

    def summary(self):
        out = "tool output %d/%d bytes" % (self.used, self.max_total)
        if self.elided:
            out += ", %d bytes elided" % self.elided
        return out

    def reset(self):
        self.used = 0
        self.elided = 0


class ChangeQueue(object):
    """A turn's planned file changes.

    Staging instead of writing gives the operator one manifest for the whole turn, and
    keeps a model that changes its mind from leaving half a plan on disk. Last write to
    the same path wins, and that is reported.
    """

    def __init__(self, max_files=MAX_QUEUED_FILES, max_bytes=MAX_QUEUED_BYTES):
        self.max_files = max_files
        self.max_bytes = max_bytes
        self.entries = []
        self.replaced = 0

    def stage(self, rel, full, content):
        if len(content) > MAX_WRITE:
            raise ValueError("refusing to stage %s: %d bytes is larger than the %d-byte limit"
                             % (rel, len(content), MAX_WRITE))
        for entry in self.entries:
            if entry["full"] == full:
                entry["content"] = content
                self.replaced += 1
                return
        total = sum(len(e["content"]) for e in self.entries)
        if len(self.entries) >= self.max_files:
            raise ValueError("refusing to stage %s: the change queue already holds %d files"
                             % (rel, self.max_files))
        if total + len(content) > self.max_bytes:
            raise ValueError("refusing to stage %s: the change queue would exceed %d bytes"
                             % (rel, self.max_bytes))
        self.entries.append({"rel": rel, "full": full, "content": content})

    def apply(self):
        """Write every staged file. Returns (wrote, failures); one failure never discards
        the rest."""
        wrote, failures = [], []
        for entry in self.entries:
            parent = os.path.dirname(entry["full"])
            try:
                if parent and not os.path.isdir(parent):
                    os.makedirs(parent)
                with open(entry["full"], "w", encoding="utf-8") as fh:
                    fh.write(entry["content"])
            except OSError as exc:
                failures.append("%s: %s" % (entry["rel"], exc))
                continue
            wrote.append("wrote %s (%d bytes)" % (entry["rel"], len(entry["content"])))
        return wrote, failures

    def summary(self):
        total = sum(len(e["content"]) for e in self.entries)
        out = "%d file(s), %d bytes" % (len(self.entries), total)
        if self.replaced:
            out += ", %d overwrite(s) of an earlier staged change" % self.replaced
        return out

    def clear(self):
        self.entries = []
        self.replaced = 0

TOOLS = (
    "rag_search: {\"tool\":\"rag_search\",\"query\":\"keywords\"}",
    "read_file: {\"tool\":\"read_file\",\"path\":\"relative.md\"}",
    "write_file: {\"tool\":\"write_file\",\"path\":\"note.txt\",\"content\":\"...\"}",
    "mqtt_publish: {\"tool\":\"mqtt_publish\",\"topic\":\"a/b\",\"payload\":\"hi\"}  (needs SLIM_MQTT_HTTP)",
)


def _jail(data_dir, rel):
    if not rel or os.path.isabs(rel) or ".." in rel.replace("\\", "/").split("/"):
        raise ValueError("path must be relative and stay in data dir")
    full = os.path.normpath(os.path.join(data_dir, rel))
    root = os.path.normpath(data_dir)
    if full != root and not full.startswith(root + os.sep):
        raise ValueError("path escapes data dir")
    return full


def _bounded(budget, text):
    """Every successful tool result passes the same budget so no tool can flood the
    conversation. Errors are returned unbudgeted on purpose: they are short, and they are
    exactly what the model needs to see."""
    return budget.add(text) if budget else text


def run_tool(name, args, conn, data_dir, mqtt_http="", queue=None, budget=None):
    args = args or {}
    if name == "rag_search":
        hits = rag_search(conn, args.get("query") or "", limit=int(args.get("limit") or 3))
        return _bounded(budget, json.dumps(hits, ensure_ascii=False))
    if name == "read_file":
        full = _jail(data_dir, args.get("path") or "")
        with open(full, "r", encoding="utf-8", errors="replace") as fh:
            return _bounded(budget, fh.read(MAX_READ))
    if name == "write_file":
        rel = args.get("path") or ""
        full = _jail(data_dir, rel)
        reason = protected_path(rel)
        if reason:
            raise ValueError(reason)
        content = args.get("content") or ""
        if len(content) > MAX_WRITE:
            raise ValueError("write over %s bytes" % MAX_WRITE)
        if queue is not None:
            # Staged, not written: the queue is applied once when the turn ends, and the
            # manifest is what the operator audits.
            queue.stage(rel, full, content)
            return _bounded(budget, "staged %s (%d bytes); it is written when this turn ends"
                            % (rel, len(content)))
        parent = os.path.dirname(full)
        if parent and not os.path.isdir(parent):
            os.makedirs(parent)
        with open(full, "w", encoding="utf-8") as fh:
            fh.write(content)
        return "wrote %s (%s bytes)" % (args.get("path"), len(content))
    if name == "mqtt_publish":
        if not mqtt_http:
            raise ValueError("mqtt disabled; set SLIM_MQTT_HTTP to an HTTP ingest URL")
        payload = json.dumps(
            {"topic": args.get("topic") or "", "payload": args.get("payload") or ""},
            ensure_ascii=False,
        ).encode("utf-8")
        req = urllib.request.Request(
            mqtt_http,
            data=payload,
            headers={"Content-Type": "application/json", "Content-Length": str(len(payload))},
            method="POST",
        )
        try:
            resp = urllib.request.urlopen(req, timeout=10)
            try:
                body = resp.read(2048)
            finally:
                resp.close()
        except urllib.error.URLError as exc:
            raise ValueError("mqtt http failed: %s" % exc.reason)
        return _bounded(budget, "mqtt http %s" % body[:200].decode("utf-8", "replace"))
    raise ValueError("unknown tool: %s" % name)


def parse_tool_call(text):
    """Return (name, args) if the model emitted a JSON tool object, else None."""
    if not text:
        return None
    start = text.find("{")
    end = text.rfind("}")
    if start < 0 or end <= start:
        return None
    try:
        obj = json.loads(text[start : end + 1])
    except ValueError:
        return None
    if not isinstance(obj, dict):
        return None
    name = obj.get("tool")
    if not name:
        return None
    return name, obj
