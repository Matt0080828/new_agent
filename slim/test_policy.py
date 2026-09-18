#!/usr/bin/env python3
"""Tests for the fail-closed tool policy and for the state-file regression.

The regression that matters here: --db defaults to <data_dir>/slim.sqlite and
write_file is jailed to <data_dir>, so a single write_file call used to be able
to overwrite the agent's own database (data_dir served as scratch space, RAG
corpus and state store at once). The database must survive that call.
"""
import json
import os
import shutil
import sqlite3
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import policy
import rag
import tools


class ProtectedPathTests(unittest.TestCase):
    def test_state_files_protected(self):
        for rel in ("slim.sqlite", "state.db", "sub/x.sqlite-wal", "x.db-journal", "a.sqlite3"):
            self.assertIsNotNone(policy.protected_path(rel), rel)

    def test_ordinary_files_allowed(self):
        for rel in ("notes.md", "sub/dir/notes.txt", "data.bin", "index.json"):
            self.assertIsNone(policy.protected_path(rel), rel)

    def test_dotfile_and_binary_protected(self):
        self.assertIsNotNone(policy.protected_path(".hidden"))
        self.assertIsNotNone(policy.protected_path("slim-agent"))

    def test_empty_name(self):
        self.assertIsNotNone(policy.protected_path(""))
        self.assertIsNotNone(policy.protected_path("sub/"))


class ApprovalTests(unittest.TestCase):
    def test_read_only_tools_always_allowed(self):
        for tool in ("rag_search", "read_file"):
            ok, why = policy.approve_tool({}, tool)
            self.assertTrue(ok, tool)
            self.assertIn("read-only", why)

    def test_fail_closed_without_opt_in(self):
        for tool in ("write_file", "mqtt_publish"):
            ok, why = policy.approve_tool({}, tool)
            self.assertFalse(ok, tool)
            self.assertIn("blocked by policy", why)

    def test_unknown_tool_denied(self):
        ok, why = policy.approve_tool({}, "delete_everything")
        self.assertFalse(ok)
        self.assertIn("not classified", why)

    def test_opt_in_is_per_tool(self):
        write_policy = {"allow_write": True}
        self.assertTrue(policy.approve_tool(write_policy, "write_file")[0])
        self.assertFalse(policy.approve_tool(write_policy, "mqtt_publish")[0])
        publish_policy = {"allow_mqtt": True}
        self.assertTrue(policy.approve_tool(publish_policy, "mqtt_publish")[0])
        self.assertFalse(policy.approve_tool(publish_policy, "write_file")[0])

    def test_human_initiated_allowed(self):
        ok, why = policy.approve_tool({}, "write_file", human=True)
        self.assertTrue(ok)
        self.assertEqual(why, "human-initiated")

    def test_prompt_paths(self):
        p = {"interactive": True}
        self.assertTrue(policy.approve_tool(p, "write_file", input_fn=lambda _q: "y")[0])
        self.assertFalse(policy.approve_tool(p, "write_file", input_fn=lambda _q: "n")[0])

        def boom(_q):
            raise EOFError()

        ok, why = policy.approve_tool(p, "write_file", input_fn=boom)
        self.assertFalse(ok)
        self.assertIn("no answer", why)


class StateFileRegressionTests(unittest.TestCase):
    """The bug: write_file could destroy the agent's own database."""

    def setUp(self):
        self.td = tempfile.mkdtemp()
        self.data = os.path.join(self.td, "data")
        os.makedirs(self.data)
        self.db = os.path.join(self.data, "slim.sqlite")  # default layout
        self.conn = rag.connect(self.db)
        rag.log_turn(self.conn, "user", "remember this")
        self.conn.commit()

    def tearDown(self):
        try:
            self.conn.close()
        except Exception:
            pass
        shutil.rmtree(self.td, ignore_errors=True)

    def _db_is_intact(self):
        """Open the file with a fresh connection and read the log back."""
        conn = sqlite3.connect(self.db)
        try:
            rows = conn.execute("SELECT COUNT(*) FROM turns").fetchone()
            return rows[0] >= 1
        finally:
            conn.close()

    def test_write_file_cannot_clobber_the_database(self):
        self.assertTrue(self._db_is_intact(), "sanity: database starts healthy")
        with self.assertRaises(ValueError) as ctx:
            tools.run_tool(
                "write_file",
                {"path": "slim.sqlite", "content": "not a database"},
                self.conn,
                self.data,
            )
        self.assertIn("state/database", str(ctx.exception))
        self.assertTrue(self._db_is_intact(), "database must be unchanged after the attempt")

    def test_write_file_cannot_clobber_via_nested_path(self):
        with self.assertRaises(ValueError):
            tools.run_tool(
                "write_file",
                {"path": "sub/../slim.sqlite", "content": "x"},
                self.conn,
                self.data,
            )
        self.assertTrue(self._db_is_intact())

    def test_ordinary_write_still_works(self):
        out = tools.run_tool(
            "write_file", {"path": "notes/ok.md", "content": "hello"}, self.conn, self.data
        )
        self.assertIn("wrote", out)
        with open(os.path.join(self.data, "notes", "ok.md"), encoding="utf-8") as fh:
            self.assertEqual(fh.read(), "hello")


class GateWiringTests(unittest.TestCase):
    """run_turn must route model tool calls through the gate, not around it."""

    def setUp(self):
        import agent

        self.agent = agent
        self.td = tempfile.mkdtemp()
        self.data = os.path.join(self.td, "data")
        os.makedirs(self.data)
        self.conn = rag.connect(os.path.join(self.data, "slim.sqlite"))
        self.calls = []
        self._orig = agent.complete_with_fallback

        def fake(primary, fallback, model, fallback_model, messages, api_key, max_tokens, timeout):
            self.calls.append(messages)
            if len(self.calls) == 1:
                return '{"tool":"write_file","path":"pwn.md","content":"hi"}'
            return "done"

        agent.complete_with_fallback = fake
        self.cfg = {
            "base_url": "http://127.0.0.1:1/v1",
            "model": "tiny",
            "data_dir": self.data,
            "skills_dir": os.path.join(self.td, "skills"),
            "docs_dir": os.path.join(self.td, "docs"),
            "max_tokens": 64,
            "timeout": 5,
            "policy": {"allow_write": False, "allow_mqtt": False, "interactive": False},
        }

    def tearDown(self):
        self.agent.complete_with_fallback = self._orig
        self.conn.close()
        shutil.rmtree(self.td, ignore_errors=True)

    def test_model_write_blocked_end_to_end(self):
        self.agent.run_turn("please write a file", self.cfg, self.conn)
        self.assertFalse(
            os.path.exists(os.path.join(self.data, "pwn.md")),
            "the gate must prevent the file from being written",
        )
        last = json.dumps(self.calls[-1], ensure_ascii=False) if self.calls else ""
        self.assertIn("blocked by policy", last, "the model must be told it was blocked")

    def test_model_write_allowed_when_enabled(self):
        self.cfg["policy"]["allow_write"] = True
        self.agent.run_turn("please write a file", self.cfg, self.conn)
        self.assertTrue(os.path.exists(os.path.join(self.data, "pwn.md")))
        with open(os.path.join(self.data, "pwn.md"), encoding="utf-8") as fh:
            self.assertEqual(fh.read(), "hi")

    def test_model_cannot_write_the_database_even_when_writes_are_allowed(self):
        self.cfg["policy"]["allow_write"] = True

        def fake(primary, fallback, model, fallback_model, messages, api_key, max_tokens, timeout):
            self.calls.append(messages)
            if len(self.calls) == 1:
                return '{"tool":"write_file","path":"slim.sqlite","content":"boom"}'
            return "done"

        self.agent.complete_with_fallback = fake
        self.agent.run_turn("overwrite your database", self.cfg, self.conn)
        conn = sqlite3.connect(os.path.join(self.data, "slim.sqlite"))
        try:
            conn.execute("SELECT COUNT(*) FROM turns").fetchone()
        finally:
            conn.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
