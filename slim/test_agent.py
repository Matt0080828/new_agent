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
        raw_req = self.rfile.read(n)
        self.server.last_body = raw_req
        self.server.n += 1
        if self.server.n == 1 and self.server.tool_first:
            content = self.server.tool_json
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
        self.httpd.tool_json = '{"tool":"rag_search","query":"OpenWrt"}'
        self.httpd.last_body = b""
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

    def test_allow_exec_on_by_default(self):
        args = agent._parse_args(["--data-dir", self.td])
        self.assertTrue(agent.build_cfg(args)["policy"]["allow_exec"], "model exec is on by default")

    def test_no_allow_exec_denies(self):
        args = agent._parse_args(["--no-allow-exec", "--data-dir", self.td])
        self.assertFalse(agent.build_cfg(args)["policy"]["allow_exec"])

    def test_allow_exec_config_and_flag_precedence(self):
        cfg_path = os.path.join(self.td, "cfg.json")
        with open(cfg_path, "w", encoding="utf-8") as fh:
            json.dump({"allow_exec": False}, fh)
        args = agent._parse_args(["--config", cfg_path, "--data-dir", self.td])
        self.assertFalse(agent.build_cfg(args)["policy"]["allow_exec"], "the config file can turn it off")
        args = agent._parse_args(["--config", cfg_path, "--allow-exec", "--data-dir", self.td])
        self.assertTrue(agent.build_cfg(args)["policy"]["allow_exec"], "an explicit flag beats the config file")

    def test_allow_exec_env_override(self):
        old = os.environ.get("SLIM_ALLOW_EXEC")
        try:
            os.environ["SLIM_ALLOW_EXEC"] = "0"
            args = agent._parse_args(["--data-dir", self.td])
            self.assertFalse(agent.build_cfg(args)["policy"]["allow_exec"])
            os.environ["SLIM_ALLOW_EXEC"] = "1"
            args = agent._parse_args(["--data-dir", self.td])
            self.assertTrue(agent.build_cfg(args)["policy"]["allow_exec"])
        finally:
            if old is None:
                os.environ.pop("SLIM_ALLOW_EXEC", None)
            else:
                os.environ["SLIM_ALLOW_EXEC"] = old

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

    def test_model_run_command_allowed_by_default(self):
        # Model-initiated execution is on by default: with the name in
        # commands.allow the tool round runs the command and the model still
        # answers in plain text.
        self.httpd.tool_first = True
        self.httpd.tool_json = '{"tool":"run_command","command":"uptime","args":""}'
        with open(os.path.join(self.td, "commands.allow"), "w", encoding="utf-8") as fh:
            fh.write("uptime\n")
        args = agent._parse_args(["--base-url", self.url, "--model", "tiny",
                                  "--data-dir", self.td, "--timeout", "5"])
        cfg = agent.build_cfg(args)
        import rag

        conn = rag.connect(cfg["db"])
        reply = agent.run_turn("check uptime", cfg, conn)
        self.assertEqual(reply, "pong")
        self.assertGreaterEqual(self.httpd.n, 2, "the tool result is fed back for the final answer")
        self.assertIn(b"exit 0", self.httpd.last_body, "the second request carries the command's exit status")
        conn.close()

    def test_model_run_command_still_needs_the_allowlist(self):
        # Default-on means model + policy gate; the allowlist is still the scope.
        self.httpd.tool_first = True
        self.httpd.tool_json = '{"tool":"run_command","command":"uptime","args":""}'
        args = agent._parse_args(["--base-url", self.url, "--model", "tiny",
                                  "--data-dir", self.td, "--timeout", "5"])
        cfg = agent.build_cfg(args)
        import rag

        conn = rag.connect(cfg["db"])
        reply = agent.run_turn("check uptime", cfg, conn)
        self.assertEqual(reply, "pong")
        self.assertGreaterEqual(self.httpd.n, 2)
        self.assertIn(b"not in the command allowlist", self.httpd.last_body,
                      "without commands.allow nothing runs, even with the default-on policy")
        conn.close()

    def test_system_prompt_includes_memory(self):
        text = agent.system_prompt("skill body", "FACT: mesh needs MTU 1500")
        self.assertIn("skill body", text)
        self.assertIn("Long-term memory", text)
        self.assertIn("FACT: mesh needs MTU 1500", text)
        # Without memory the section is absent entirely (no dead prompt lines).
        self.assertNotIn("Long-term memory", agent.system_prompt("skill body", ""))

    def test_run_turn_injects_memory_into_the_prompt(self):
        with open(os.path.join(self.td, "memory.md"), "w", encoding="utf-8") as fh:
            fh.write("FACT: this box runs CAP-RE mesh\n")
        args = agent._parse_args(["--base-url", self.url, "--model", "tiny",
                                  "--data-dir", self.td, "--timeout", "5"])
        cfg = agent.build_cfg(args)
        import rag

        conn = rag.connect(cfg["db"])
        reply = agent.run_turn("hello", cfg, conn)
        self.assertEqual(reply, "pong")
        self.assertIn(b"CAP-RE mesh", self.httpd.last_body, "the memory fact is in the system prompt")
        conn.close()

    def test_chat_error_raises(self):
        with self.assertRaises(SlimError):
            agent.chat_completion("http://127.0.0.1:1", "tiny", [{"role": "user", "content": "x"}], timeout=1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
