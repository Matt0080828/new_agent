#!/usr/bin/env python3
"""Tests for the credential policy: no token over plaintext http:// without an
explicit opt-in, and no token on a redirect that leaves the origin.

Local servers only; no network.
"""
import os
import sys
import threading
import unittest
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import net


class AuthHeaderTests(unittest.TestCase):
    def test_no_token_is_empty(self):
        self.assertEqual(net.auth_header("http://host/v1", ""), {})
        self.assertEqual(net.auth_header("https://host/v1", None), {})

    def test_https_carries_the_token(self):
        self.assertEqual(net.auth_header("https://host/v1", "k"),
                         {"Authorization": "Bearer k"})

    def test_plaintext_is_refused_by_default(self):
        with self.assertRaises(ValueError):
            net.auth_header("http://host/v1", "k")

    def test_plaintext_allowed_when_opted_in(self):
        self.assertEqual(
            net.auth_header("http://host/v1", "k", allow_plaintext=True),
            {"Authorization": "Bearer k"})

    def test_scheme_check_is_case_insensitive(self):
        self.assertEqual(net.auth_header("HTTPS://host/v1", "k"),
                         {"Authorization": "Bearer k"})


def start(handler):
    srv = HTTPServer(("127.0.0.1", 0), handler)
    t = threading.Thread(target=srv.serve_forever)
    t.daemon = True
    t.start()
    return srv


class RedirectTests(unittest.TestCase):
    def _request(self, url, authorization="Bearer secret"):
        req = urllib.request.Request(url, headers={"Authorization": authorization})
        resp = net.open_url(req, timeout=5)
        try:
            resp.read()
        finally:
            resp.close()

    def test_token_is_dropped_on_a_cross_origin_redirect(self):
        seen = []

        class Target(BaseHTTPRequestHandler):
            def do_GET(self):
                seen.append(self.headers.get("Authorization"))
                self.send_response(200)
                self.send_header("Content-Length", "2")
                self.end_headers()
                self.wfile.write(b"ok")

            def log_message(self, *a):
                pass

        target = start(Target)
        self.addCleanup(target.shutdown)
        tport = target.server_address[1]

        class Redirect(BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(302)
                self.send_header("Location", "http://127.0.0.1:%d/final" % tport)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *a):
                pass

        redirect = start(Redirect)
        self.addCleanup(redirect.shutdown)

        self._request("http://127.0.0.1:%d/go" % redirect.server_address[1])
        self.assertEqual(seen, [None], "the token must not follow a cross-origin redirect")

    def test_token_survives_a_same_origin_redirect(self):
        seen = []

        class Same(BaseHTTPRequestHandler):
            def do_GET(self):
                if self.path == "/go":
                    self.send_response(302)
                    self.send_header("Location", "/final")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                seen.append(self.headers.get("Authorization"))
                self.send_response(200)
                self.send_header("Content-Length", "2")
                self.end_headers()
                self.wfile.write(b"ok")

            def log_message(self, *a):
                pass

        srv = start(Same)
        self.addCleanup(srv.shutdown)
        self._request("http://127.0.0.1:%d/go" % srv.server_address[1])
        self.assertEqual(seen, ["Bearer secret"])


class MqttAuthTests(unittest.TestCase):
    def test_mqtt_token_is_refused_over_plaintext(self):
        import tools
        with self.assertRaises(ValueError):
            tools.run_tool("mqtt_publish", {"topic": "a", "payload": "b"}, None,
                           "/tmp", "http://host/mqtt", mqtt_token="k")

    def test_mqtt_without_token_builds_no_header(self):
        self.assertEqual(net.auth_header("http://host/mqtt", ""), {})


if __name__ == "__main__":
    unittest.main()
