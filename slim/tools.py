"""Whitelist tools. Paths stay under data_dir. No shell."""
import json
import os
import urllib.error
import urllib.request

from rag import search as rag_search

MAX_WRITE = 8 * 1024
MAX_READ = 16 * 1024

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


def run_tool(name, args, conn, data_dir, mqtt_http=""):
    args = args or {}
    if name == "rag_search":
        hits = rag_search(conn, args.get("query") or "", limit=int(args.get("limit") or 3))
        return json.dumps(hits, ensure_ascii=False)
    if name == "read_file":
        full = _jail(data_dir, args.get("path") or "")
        with open(full, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read(MAX_READ)
    if name == "write_file":
        full = _jail(data_dir, args.get("path") or "")
        content = args.get("content") or ""
        if len(content) > MAX_WRITE:
            raise ValueError("write over %s bytes" % MAX_WRITE)
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
        return "mqtt http %s" % body[:200].decode("utf-8", "replace")
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
