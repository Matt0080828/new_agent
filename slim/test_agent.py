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

import agent
from error import SlimError


class _Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        return

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        self.rfile.read(n)
        self.server.n += 1
        if self.server.n == 1 and self.server.tool_first:
            content = '{"tool":"rag_search","query":"OpenWrt"}'
        else:
            content = "pong"
        payload = {"choices": [{"message": {"role": "assistant", "content": content}}]}
        raw = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)


class SlimAgentTests(unittest.TestCase):
    def setUp(self):
        self.httpd = HTTPServer(("127.0.0.1", 0), _Handler)
        self.httpd.n = 0
        self.httpd.tool_first = False
        self.thread = threading.Thread(target=self.httpd.serve_forever)
        self.thread.daemon = True
        self.thread.start()
        self.port = self.httpd.server_address[1]
        self.url = "http://127.0.0.1:%s/v1" % self.port
        self.td = tempfile.mkdtemp()

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=5)
        shutil.rmtree(self.td)

    def test_chat_completion_roundtrip(self):
        text = agent.chat_completion(self.url, "tiny", [{"role": "user", "content": "ping"}], max_tokens=16, timeout=5)
        self.assertEqual(text, "pong")

    def test_once_cli(self):
        rc = agent.main(["--base-url", self.url, "--model", "tiny", "--once", "hello", "--timeout", "5", "--data-dir", self.td])
        self.assertEqual(rc, 0)

    def test_missing_base_url_exits(self):
        with self.assertRaises(SystemExit) as ctx:
            agent.main(["--model", "tiny", "--data-dir", self.td])
        self.assertEqual(ctx.exception.code, 2)

    def test_tool_round(self):
        self.httpd.tool_first = True
        skills = os.path.join(HERE, "skills")
        cfg = {
            "base_url": self.url,
            "fallback_url": "",
            "model": "tiny",
            "fallback_model": "",
            "api_key": None,
            "max_tokens": 32,
            "timeout": 5,
            "data_dir": self.td,
            "skills_dir": skills,
            "docs_dir": os.path.join(self.td, "docs"),
            "mqtt_http": "",
            "db": os.path.join(self.td, "slim.sqlite"),
        }
        os.makedirs(cfg["docs_dir"])
        with open(os.path.join(cfg["docs_dir"], "x.md"), "w", encoding="utf-8") as fh:
            fh.write("OpenWrt musl notes\n")
        import rag

        conn = rag.connect(cfg["db"])
        rag.ingest_tree(conn, cfg["docs_dir"], prefix="doc")
        reply = agent.run_turn("what about OpenWrt", cfg, conn)
        self.assertEqual(reply, "pong")
        self.assertGreaterEqual(self.httpd.n, 2)
        conn.close()

    def test_chat_error_raises(self):
        with self.assertRaises(SlimError):
            agent.chat_completion("http://127.0.0.1:1", "tiny", [{"role": "user", "content": "x"}], timeout=1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
