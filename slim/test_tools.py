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


if __name__ == "__main__":
    unittest.main(verbosity=2)
