"""SQLite FTS5 keyword RAG. No embeddings."""
import os
import sqlite3

SCHEMA = """
CREATE VIRTUAL TABLE IF NOT EXISTS docs USING fts5(path, body, tokenize='unicode61');
CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT);
CREATE TABLE IF NOT EXISTS turns (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  role TEXT NOT NULL,
  content TEXT NOT NULL,
  ts DATETIME DEFAULT CURRENT_TIMESTAMP
);
"""


def connect(db_path):
    parent = os.path.dirname(db_path)
    if parent and not os.path.isdir(parent):
        os.makedirs(parent)
    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA busy_timeout=3000")
    conn.executescript(SCHEMA)
    return conn


def ingest_tree(conn, root, prefix=""):
    if not os.path.isdir(root):
        return 0
    n = 0
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            if not name.endswith((".md", ".txt")):
                continue
            full = os.path.join(dirpath, name)
            rel = os.path.join(prefix, os.path.relpath(full, root)).replace("\\", "/")
            with open(full, "r", encoding="utf-8", errors="replace") as fh:
                body = fh.read(64 * 1024)
            conn.execute("DELETE FROM docs WHERE path = ?", (rel,))
            conn.execute("INSERT INTO docs(path, body) VALUES (?, ?)", (rel, body))
            n += 1
    conn.commit()
    return n


def search(conn, query, limit=3):
    q = (query or "").strip()
    if not q:
        return []
    # FTS5 MATCH; quote user text as a phrase to avoid syntax errors.
    phrase = '"' + q.replace('"', " ") + '"'
    rows = conn.execute(
        "SELECT path, snippet(docs, 1, '[', ']', ' … ', 12) FROM docs WHERE docs MATCH ? LIMIT ?",
        (phrase, int(limit)),
    ).fetchall()
    if rows:
        return [{"path": r[0], "snippet": r[1]} for r in rows]
    like = "%" + q.replace("%", "")[:80] + "%"
    rows = conn.execute(
        "SELECT path, substr(body, 1, 240) FROM docs WHERE body LIKE ? LIMIT ?",
        (like, int(limit)),
    ).fetchall()
    return [{"path": r[0], "snippet": r[1]} for r in rows]


def log_turn(conn, role, content):
    conn.execute(
        "INSERT INTO turns(role, content) VALUES (?, ?)",
        (role, (content or "")[:8000]),
    )
    conn.commit()
