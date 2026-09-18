#!/usr/bin/env python3
import json
import os
import shutil
import sys
import tempfile
import threading
import unittest

try:
    from http.server import BaseHTTPRequestHandler, HTTPServer
except ImportError:
    from BaseHTTPServer import BaseHTTPRequestHandler, HTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import rag
import tools


class ToolsTests(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.mkdtemp()
        self.data = os.path.join(self.td, "data")
        os.makedirs(self.data)
        self.conn = rag.connect(os.path.join(self.td, "t.sqlite"))
        with open(os.path.join(self.data, "a.txt"), "w", encoding="utf-8") as fh:
            fh.write("hello")
        rag.ingest_tree(self.conn, self.data, prefix="data")

    def tearDown(self):
        self.conn.close()
        shutil.rmtree(self.td)

    def test_read_write_jail(self):
        out = tools.run_tool("write_file", {"path": "b.txt", "content": "x"}, self.conn, self.data)
        self.assertIn("wrote", out)
        self.assertEqual(tools.run_tool("read_file", {"path": "b.txt"}, self.conn, self.data), "x")
        with self.assertRaises(ValueError):
            tools.run_tool("read_file", {"path": "../etc/passwd"}, self.conn, self.data)

    def test_rag_search_tool(self):
        raw = tools.run_tool("rag_search", {"query": "hello"}, self.conn, self.data)
        hits = json.loads(raw)
        self.assertTrue(hits)

    def test_mqtt_disabled(self):
        with self.assertRaises(ValueError):
            tools.run_tool("mqtt_publish", {"topic": "a", "payload": "1"}, self.conn, self.data, mqtt_http="")

    def test_parse_tool_call(self):
        got = tools.parse_tool_call('use {"tool":"rag_search","query":"cpe"}')
        self.assertEqual(got[0], "rag_search")
        self.assertEqual(got[1]["query"], "cpe")
        self.assertIsNone(tools.parse_tool_call("plain answer"))


class MqttHttpTests(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.mkdtemp()
        self.data = os.path.join(self.td, "data")
        os.makedirs(self.data)
        self.conn = rag.connect(os.path.join(self.td, "t.sqlite"))

        class H(BaseHTTPRequestHandler):
            def log_message(self, fmt, *args):
                return

            def do_POST(self):
                n = int(self.headers.get("Content-Length", "0"))
                self.server.last = self.rfile.read(n)
                body = b"ok"
                self.send_response(200)
                self.send_header("Content-Length", "2")
                self.end_headers()
                self.wfile.write(body)

        self.httpd = HTTPServer(("127.0.0.1", 0), H)
        self.th = threading.Thread(target=self.httpd.serve_forever)
        self.th.daemon = True
        self.th.start()
        self.url = "http://127.0.0.1:%s/ingest" % self.httpd.server_address[1]

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.th.join(timeout=5)
        self.conn.close()
        shutil.rmtree(self.td)

    def test_mqtt_http(self):
        out = tools.run_tool(
            "mqtt_publish",
            {"topic": "t/a", "payload": "1"},
            self.conn,
            self.data,
            mqtt_http=self.url,
        )
        self.assertIn("mqtt http", out)
        body = json.loads(self.httpd.last.decode("utf-8"))
        self.assertEqual(body["topic"], "t/a")
class ToolBudgetTests(unittest.TestCase):
    def test_under_the_budget_passes_through(self):
        b = tools.ToolBudget(100)
        self.assertEqual(b.add("short"), "short")
        self.assertEqual(b.used, 5)

    def test_over_the_budget_is_trimmed_with_a_marker(self):
        b = tools.ToolBudget(100)
        b.add("x" * 5)
        trimmed = b.add("y" * 200)
        self.assertLess(len(trimmed), 200)
        self.assertIn("[truncated", trimmed)
        self.assertEqual(b.used, 100)
        self.assertEqual(b.elided, 105)

    def test_exhausted_budget_returns_an_elision_marker(self):
        b = tools.ToolBudget(10)
        b.add("z" * 10)
        self.assertIn("omitted", b.add("more"))
        self.assertIn("10/10 bytes", b.summary())
        self.assertIn("elided", b.summary())

    def test_reset_makes_it_usable_again(self):
        b = tools.ToolBudget(10)
        b.add("z" * 10)
        b.reset()
        self.assertEqual(b.add("again"), "again")

    def test_run_tool_applies_the_budget(self):
        b = tools.ToolBudget(40)
        tmp = tempfile.mkdtemp(prefix="slim-budget-")
        self.addCleanup(shutil.rmtree, tmp, True)
        with open(os.path.join(tmp, "big.md"), "w", encoding="utf-8") as fh:
            fh.write("q" * 500)
        out = tools.run_tool("read_file", {"path": "big.md"}, None, tmp, budget=b)
        self.assertIn("[truncated", out)
        self.assertLessEqual(b.used, 40)


class ChangeQueueTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="slim-queue-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_model_write_is_staged_not_written(self):
        q = tools.ChangeQueue()
        out = tools.run_tool("write_file", {"path": "s.md", "content": "one"}, None, self.tmp,
                             queue=q)
        self.assertTrue(out.startswith("staged"), out)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "s.md")))
        self.assertEqual(len(q.entries), 1)

    def test_apply_writes_and_reports(self):
        q = tools.ChangeQueue()
        tools.run_tool("write_file", {"path": "a.md", "content": "first"}, None, self.tmp, queue=q)
        tools.run_tool("write_file", {"path": "a.md", "content": "second"}, None, self.tmp, queue=q)
        tools.run_tool("write_file", {"path": "sub/b.md", "content": "third"}, None, self.tmp, queue=q)
        self.assertEqual(len(q.entries), 2, "the same path is not queued twice")
        self.assertEqual(q.replaced, 1, "overwriting a staged change is counted")
        wrote, failures = q.apply()
        self.assertEqual(failures, [])
        self.assertEqual(len(wrote), 2)
        self.assertIn("wrote a.md (6 bytes)", wrote[0])
        with open(os.path.join(self.tmp, "a.md"), encoding="utf-8") as fh:
            self.assertEqual(fh.read(), "second", "the last staged content wins")
        with open(os.path.join(self.tmp, "sub", "b.md"), encoding="utf-8") as fh:
            self.assertEqual(fh.read(), "third")

    def test_per_file_cap(self):
        q = tools.ChangeQueue()
        with self.assertRaises(ValueError) as ctx:
            q.stage("big.md", os.path.join(self.tmp, "big.md"), "x" * (tools.MAX_WRITE + 1))
        self.assertIn("larger than", str(ctx.exception))

    def test_file_count_cap(self):
        q = tools.ChangeQueue(max_files=2)
        q.stage("a.md", os.path.join(self.tmp, "a.md"), "x")
        q.stage("b.md", os.path.join(self.tmp, "b.md"), "x")
        with self.assertRaises(ValueError) as ctx:
            q.stage("c.md", os.path.join(self.tmp, "c.md"), "x")
        self.assertIn("already holds", str(ctx.exception))

    def test_byte_cap(self):
        q = tools.ChangeQueue(max_bytes=10)
        q.stage("a.md", os.path.join(self.tmp, "a.md"), "x" * 8)
        with self.assertRaises(ValueError) as ctx:
            q.stage("b.md", os.path.join(self.tmp, "b.md"), "x" * 8)
        self.assertIn("would exceed", str(ctx.exception))

    def test_protected_paths_are_refused_before_staging(self):
        q = tools.ChangeQueue()
        with self.assertRaises(ValueError):
            tools.run_tool("write_file", {"path": "sessions/s.jsonl", "content": "x"}, None,
                           self.tmp, queue=q)
        self.assertEqual(q.entries, [], "nothing is staged for a protected path")

    def test_a_failed_apply_keeps_the_others(self):
        q = tools.ChangeQueue()
        q.stage("ok.md", os.path.join(self.tmp, "ok.md"), "fine")
        q.stage("blocked.md", os.path.join(self.tmp, "blocked.md"), "also fine")
        with open(os.path.join(self.tmp, "blocked"), "w", encoding="utf-8") as fh:
            fh.write("i am a file, not a directory")
        q.stage("inner.md", os.path.join(self.tmp, "blocked", "inner.md"), "nope")
        wrote, failures = q.apply()
        # The two writable files still land; only the one behind a file-as-directory fails.
        self.assertEqual(len(wrote), 2, wrote)
        self.assertEqual(len(failures), 1, failures)
        self.assertIn("inner.md", failures[0])
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "ok.md")))
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "blocked.md")))


if __name__ == "__main__":
    unittest.main(verbosity=2)
