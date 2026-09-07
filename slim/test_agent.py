#!/usr/bin/env python3
"""Stdlib tests for slim/agent.py (no pip)."""
import json
import os
import sys
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


class _Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        return

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        self.server.last_body = body
        self.server.last_path = self.path
        payload = {
            "choices": [{"message": {"role": "assistant", "content": "pong"}}]
        }
        raw = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)


class SlimAgentTests(unittest.TestCase):
    def setUp(self):
        self.httpd = HTTPServer(("127.0.0.1", 0), _Handler)
        self.thread = threading.Thread(target=self.httpd.serve_forever)
        self.thread.daemon = True
        self.thread.start()
        self.port = self.httpd.server_address[1]

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=5)

    def test_chat_completion_roundtrip(self):
        url = "http://127.0.0.1:%s/v1" % self.port
        text = agent.chat_completion(url, "tiny", [{"role": "user", "content": "ping"}], max_tokens=16, timeout=5)
        self.assertEqual(text, "pong")
        body = json.loads(self.httpd.last_body.decode("utf-8"))
        self.assertEqual(body["model"], "tiny")
        self.assertEqual(body["max_tokens"], 16)
        self.assertFalse(body["stream"])

    def test_once_cli(self):
        rc = agent.main(
            [
                "--base-url",
                "http://127.0.0.1:%s/v1" % self.port,
                "--model",
                "tiny",
                "--once",
                "hello",
                "--timeout",
                "5",
            ]
        )
        self.assertEqual(rc, 0)

    def test_missing_base_url_exits(self):
        with self.assertRaises(SystemExit) as ctx:
            agent.main(["--model", "tiny"])
        self.assertEqual(ctx.exception.code, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
