#!/usr/bin/env python3
"""Tests for the bounded retry policy and for "mutations are never retried".

Everything here is deterministic: the clock and the sleep function are injected,
and the HTTP cases use a local server (no network).
"""
import json
import os
import shutil
import sys
import threading
import unittest
import urllib.error
from http.server import BaseHTTPRequestHandler, HTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import agent
import retry
import tools


class ClassificationTests(unittest.TestCase):
    def test_status(self):
        self.assertTrue(retry.is_retryable_status(429))
        self.assertTrue(retry.is_retryable_status(500))
        self.assertTrue(retry.is_retryable_status(503))
        self.assertFalse(retry.is_retryable_status(400))
        self.assertFalse(retry.is_retryable_status(404))
        self.assertFalse(retry.is_retryable_status(200))

    def test_exceptions(self):
        self.assertTrue(retry.is_retryable_exception(urllib.error.URLError("reset")))
        self.assertTrue(retry.is_retryable_exception(OSError("broken pipe")))
        self.assertTrue(retry.is_retryable_exception(
            urllib.error.HTTPError("u", 503, "busy", {}, None)))
        self.assertFalse(retry.is_retryable_exception(
            urllib.error.HTTPError("u", 400, "bad", {}, None)))
        self.assertFalse(retry.is_retryable_exception(
            urllib.error.URLError(ValueError("unknown url type: 'https'"))))
        self.assertFalse(retry.is_retryable_exception(ValueError("bad config")))
        # unrecognised exception types are not retried
        self.assertFalse(retry.is_retryable_exception(KeyError("nope")))

    def test_delay(self):
        self.assertEqual(retry.retry_delay_seconds(1, 0.5, 4.0), 0.5)
        self.assertEqual(retry.retry_delay_seconds(2, 0.5, 4.0), 1.0)
        self.assertEqual(retry.retry_delay_seconds(3, 0.5, 4.0), 2.0)
        self.assertEqual(retry.retry_delay_seconds(9, 0.5, 4.0), 4.0)


class LoopTests(unittest.TestCase):
    def setUp(self):
        self.sleeps = []
        self.logs = []

    def _sleep(self, seconds):
        self.sleeps.append(seconds)

    def _log(self, line):
        self.logs.append(line)

    def test_recovers_after_two_5xx(self):
        calls = []

        def fn():
            calls.append(1)
            if len(calls) < 3:
                raise urllib.error.HTTPError("u", 500, "boom", {}, None)
            return "ok"

        out = retry.run_with_retry(fn, attempts=3, base_delay=0.5, sleep=self._sleep, log=self._log)
        self.assertEqual(out, "ok")
        self.assertEqual(len(calls), 3, "three attempts")
        self.assertEqual(self.sleeps, [0.5, 1.0], "backoff 0.5 then 1.0")
        self.assertIn("status" if False else "retryable", " ".join(self.logs))

    def test_permanent_error_is_not_retried(self):
        calls = []

        def fn():
            calls.append(1)
            raise urllib.error.HTTPError("u", 400, "bad request", {}, None)

        with self.assertRaises(urllib.error.HTTPError):
            retry.run_with_retry(fn, attempts=3, sleep=self._sleep, log=self._log)
        self.assertEqual(len(calls), 1, "one attempt only")
        self.assertEqual(self.sleeps, [], "no sleeping for a permanent error")

    def test_exhausts_attempts(self):
        calls = []

        def fn():
            calls.append(1)
            raise urllib.error.HTTPError("u", 503, "busy", {}, None)

        with self.assertRaises(urllib.error.HTTPError):
            retry.run_with_retry(fn, attempts=3, base_delay=0.1, sleep=self._sleep, log=self._log)
        self.assertEqual(len(calls), 3)
        self.assertEqual(len(self.sleeps), 2)

    def test_budget_stops_early(self):
        calls = []
        now = [0.0]

        def clock():
            return now[0]

        def sleep(seconds):
            self.sleeps.append(seconds)
            now[0] += seconds  # let the fake clock advance with the sleeps

        def fn():
            calls.append(1)
            raise urllib.error.URLError("reset")

        with self.assertRaises(urllib.error.URLError):
            retry.run_with_retry(fn, attempts=10, base_delay=1.0, max_delay=8.0,
                                 max_total=5.0, sleep=sleep, clock=clock, log=self._log)
        self.assertLess(len(calls), 10, "the budget stops the loop before attempts run out")
        self.assertTrue(any("budget" in line for line in self.logs), "the budget stop is logged")

    def test_single_attempt_never_sleeps(self):
        def fn():
            raise urllib.error.URLError("reset")

        with self.assertRaises(urllib.error.URLError):
            retry.run_with_retry(fn, attempts=1, sleep=self._sleep)
        self.assertEqual(self.sleeps, [])


class ChatRetryTests(unittest.TestCase):
    """agent.chat_completion must retry the transport, not the parsing."""

    def setUp(self):
        self.calls = 0
        self.orig = agent._post_once
        self.sleeps = []
        self.orig_sleep = retry.time.sleep
        retry.time.sleep = lambda s: self.sleeps.append(s)

    def tearDown(self):
        agent._post_once = self.orig
        retry.time.sleep = self.orig_sleep

    def _body(self):
        return json.dumps({"choices": [{"message": {"content": "hello"}}]}).encode("utf-8")

    def test_retries_transport_then_succeeds(self):
        def fake(url, data, headers, timeout):
            self.calls += 1
            if self.calls < 3:
                raise urllib.error.URLError("connection refused")
            return self._body()

        agent._post_once = fake
        out = agent.chat_completion("http://127.0.0.1:1", "tiny", [{"role": "user", "content": "hi"}],
                                    retry_policy={"attempts": 3, "base_delay": 0.0})
        self.assertEqual(out, "hello")
        self.assertEqual(self.calls, 3)

    def test_permanent_http_error_is_not_retried(self):
        def fake(url, data, headers, timeout):
            self.calls += 1
            raise urllib.error.HTTPError(url, 400, "bad", {}, None)

        agent._post_once = fake
        with self.assertRaises(agent.SlimError):
            agent.chat_completion("http://127.0.0.1:1", "tiny", [{"role": "user", "content": "hi"}],
                                  retry_policy={"attempts": 5, "base_delay": 0.0})
        self.assertEqual(self.calls, 1, "a 400 must not be retried")


class MutationNotRetriedTests(unittest.TestCase):
    """mqtt_publish is a mutation: exactly one request, even on a 5xx."""

    def setUp(self):
        self.requests = 0
        self.td = None

        class H(BaseHTTPRequestHandler):
            def log_message(self, fmt, *args):
                return

            def do_POST(self):
                outer.requests += 1
                n = int(self.headers.get("Content-Length", "0"))
                self.rfile.read(n)
                self.send_response(500)
                self.send_header("Content-Length", "2")
                self.end_headers()
                self.wfile.write(b"no")

        outer = self
        self.httpd = HTTPServer(("127.0.0.1", 0), H)
        self.th = threading.Thread(target=self.httpd.serve_forever)
        self.th.daemon = True
        self.th.start()
        self.url = "http://127.0.0.1:%s/ingest" % self.httpd.server_address[1]

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.th.join(timeout=5)

    def test_single_request_on_500(self):
        import rag

        conn = rag.connect(":memory:")
        try:
            with self.assertRaises(ValueError):
                tools.run_tool("mqtt_publish", {"topic": "a/b", "payload": "1"}, conn, ".",
                               mqtt_http=self.url)
        finally:
            conn.close()
        self.assertEqual(self.requests, 1, "a publish must not be retried")


if __name__ == "__main__":
    unittest.main(verbosity=2)
