"""Tests for the SSE parser and the streaming transport.

The parser tests drive every possible chunk split in-process, so they stay
deterministic; the transport tests inject a fake opener and an injected clock/sleep
so no test ever waits on a real backoff. One test still talks to a real socket, to
prove the wiring rather than only the logic.
"""
import io
import json
import os
import queue
import sys
import threading
import time
import unittest
import urllib.error
from http.server import BaseHTTPRequestHandler, HTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from sse import SseParser, assemble, delta_content          # noqa: E402
from stream import StreamResult, read_stream, stream_completion  # noqa: E402


def chunk_for(text):
    return "data: " + json.dumps({"choices": [{"delta": {"content": text}}]}) + "\n\n"


BODY = (chunk_for("Hel") + chunk_for("lo") + chunk_for(" SSE") + "data: [DONE]\n\n")


class TestSseParser(unittest.TestCase):
    def test_single_event(self):
        p = SseParser()
        p.feed(chunk_for("hi"))
        payload = p.next_payload()
        self.assertIsNotNone(payload)
        self.assertEqual(delta_content(payload), "hi")
        self.assertIsNone(p.next_payload())
        self.assertFalse(p.done())

    def test_every_chunk_boundary(self):
        # One byte at a time: splits inside "data:", inside the JSON, between the
        # blank-line pair - every position a socket could break the stream at.
        p = SseParser()
        for i in range(len(BODY)):
            p.feed(BODY[i])
        p.flush()
        text = []
        while True:
            payload = p.next_payload()
            if payload is None:
                break
            if delta_content(payload):
                text.append(delta_content(payload))
        self.assertEqual("".join(text), "Hello SSE")
        self.assertTrue(p.done())

    def test_two_events_in_one_chunk(self):
        p = SseParser()
        p.feed("data: {\"a\":1}\n\ndata: {\"b\":2}\n\n")
        self.assertEqual(p.next_payload(), "{\"a\":1}")
        self.assertEqual(p.next_payload(), "{\"b\":2}")
        self.assertIsNone(p.next_payload())

    def test_crlf_and_noise_lines(self):
        p = SseParser()
        p.feed(": keep-alive\r\nevent: message\r\nid: 42\r\nretry: 1000\r\n"
               "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\r\n\r\n")
        payload = p.next_payload()
        self.assertIsNotNone(payload)
        self.assertEqual(delta_content(payload), "x")
        self.assertIsNone(p.next_payload())

    def test_multiline_data_joined(self):
        p = SseParser()
        p.feed("data: line one\ndata: line two\n\n")
        self.assertEqual(p.next_payload(), "line one\nline two")

    def test_done_marker(self):
        p = SseParser()
        p.feed("data: [DONE]\n\n")
        self.assertIsNone(p.next_payload())
        self.assertTrue(p.done())

    def test_flush_processes_leftover_without_newline(self):
        # The regression that a stream ending without a trailing newline used to
        # cause: the leftover stayed in the buffer and corrupted the next parse.
        p = SseParser()
        p.feed("data: {\"choices\":[{\"delta\":{\"content\":\"last\"}}]}")
        self.assertIsNone(p.next_payload())
        p.flush()
        self.assertEqual(delta_content(p.next_payload()), "last")
        p.feed("data: [DONE]\n\n")
        self.assertTrue(p.done(), "leftover must not leak into the next parse")


class TestDeltaContent(unittest.TestCase):
    def test_delta_form(self):
        self.assertEqual(delta_content("{\"choices\":[{\"delta\":{\"content\":\"abc\"}}]}"), "abc")

    def test_message_form(self):
        self.assertEqual(delta_content("{\"choices\":[{\"message\":{\"content\":\"full\"}}]}"), "full")

    def test_nulls_and_noise(self):
        self.assertIsNone(delta_content("{\"choices\":[{\"delta\":{\"content\":null}}]}"))
        self.assertIsNone(delta_content("{\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}"))
        self.assertIsNone(delta_content("not json"))
        self.assertIsNone(delta_content(""))

    def test_assemble_whole_body(self):
        self.assertEqual(assemble(BODY), "Hello SSE")

    def test_assemble_plain_json_is_not_a_stream(self):
        self.assertEqual(assemble("{\"choices\":[{\"message\":{\"content\":\"x\"}}]}"), "")


class FakeResponse(object):
    """Minimal read()/close() surface; `read` ignores n on purpose."""

    def __init__(self, chunks, raise_after=None):
        self._chunks = list(chunks)
        self._raise_after = raise_after
        self.closed = False

    def read(self, n=4096):
        if self._chunks:
            return self._chunks.pop(0)
        if self._raise_after is not None:
            exc, self._raise_after = self._raise_after, None
            raise exc
        return b""

    def close(self):
        self.closed = True


class FakeOpener(object):
    def __init__(self, results):
        self.results = list(results)
        self.calls = 0

    def __call__(self, request, timeout=None):
        self.calls += 1
        item = self.results.pop(0) if self.results else RuntimeError("no more responses")
        if isinstance(item, Exception):
            raise item
        return item


def http_error(code, body="busy"):
    return urllib.error.HTTPError("http://x/v1/chat/completions", code, "err", {},
                                  io.BytesIO(body.encode("utf-8")))


class TestReadStream(unittest.TestCase):
    def test_chunk_sizes_do_not_change_the_result(self):
        for size in (1, 3, 7, 64, 4096):
            body = BODY.encode("utf-8")
            chunks = [body[i:i + size] for i in range(0, len(body), size)]
            seen = []
            text, deltas, saw_done = read_stream(FakeResponse(chunks), seen.append)
            self.assertEqual(text, "Hello SSE", "size=%d" % size)
            self.assertEqual("".join(seen), "Hello SSE")
            self.assertEqual(deltas, 3)
            self.assertTrue(saw_done)

    def test_missing_done_is_reported(self):
        text, deltas, saw_done = read_stream(FakeResponse([chunk_for("x").encode("utf-8")]), None)
        self.assertEqual(text, "x")
        self.assertFalse(saw_done)

    def test_oversized_stream_is_refused(self):
        body = (chunk_for("x" * 200) * 20).encode("utf-8")
        with self.assertRaises(ValueError):
            read_stream(FakeResponse([body]), None, max_bytes=512)


class TestStreamCompletion(unittest.TestCase):
    def policy(self):
        return {"attempts": 3, "base_delay": 0.0, "max_total": 0.0}

    def test_retries_before_the_first_delta(self):
        opener = FakeOpener([http_error(500), FakeResponse([BODY.encode("utf-8")])])
        slept = []
        seen = []
        result = stream_completion("http://x/v1/chat/completions", {"stream": True},
                                   on_delta=seen.append, retry_policy=self.policy(),
                                   sleep=slept.append, opener=opener)
        self.assertEqual(result.text, "Hello SSE")
        self.assertEqual("".join(seen), "Hello SSE")
        self.assertEqual(opener.calls, 2)
        self.assertEqual(len(slept), 1, "one backoff between the two attempts")

    def test_never_retries_after_a_delta(self):
        # First attempt yields one delta, then the socket dies. The retry must not
        # happen: the operator has already seen output.
        opener = FakeOpener([
            FakeResponse([chunk_for("one").encode("utf-8")], raise_after=OSError("connection reset")),
            FakeResponse([BODY.encode("utf-8")]),
        ])
        slept = []
        seen = []
        with self.assertRaises(Exception) as ctx:
            stream_completion("http://x/v1/chat/completions", {"stream": True},
                              on_delta=seen.append, retry_policy=self.policy(),
                              sleep=slept.append, opener=opener)
        self.assertIn("connection reset", str(ctx.exception))
        self.assertEqual(seen, ["one"], "the delta was delivered exactly once")
        self.assertEqual(opener.calls, 1, "no second attempt")
        self.assertEqual(slept, [], "no backoff after output")

    def test_permanent_http_error_is_not_retried(self):
        opener = FakeOpener([http_error(400, "bad request"), FakeResponse([BODY.encode("utf-8")])])
        with self.assertRaises(Exception):
            stream_completion("http://x/v1/chat/completions", {"stream": True},
                              retry_policy=self.policy(), sleep=lambda s: None, opener=opener)
        self.assertEqual(opener.calls, 1)

    def test_error_status_is_reported_and_emits_nothing(self):
        """A 400 from a streaming endpoint must raise naming the status, and emit nothing.

        The C++ client used to print the raw response (status line, headers and body)
        and store it as the assistant answer; both clients must instead surface the
        failure and leave the session untouched.
        """
        seen = []
        opener = FakeOpener([http_error(400, '{"error":{"message":"Failed to load model"}}')])
        with self.assertRaises(Exception) as ctx:
            stream_completion("http://x/v1/chat/completions", {"stream": True},
                              on_delta=seen.append, retry_policy=self.policy(),
                              sleep=lambda s: None, opener=opener)
        self.assertIn("HTTP 400", str(ctx.exception))
        self.assertEqual(seen, [], "a failed request must not emit deltas")
        self.assertEqual(opener.calls, 1, "a 400 is permanent and is never retried")

    def test_transport_error_is_retried(self):
        opener = FakeOpener([urllib.error.URLError("connection refused"),
                             FakeResponse([BODY.encode("utf-8")])])
        result = stream_completion("http://x/v1/chat/completions", {"stream": True},
                                   retry_policy=self.policy(), sleep=lambda s: None, opener=opener)
        self.assertEqual(result.text, "Hello SSE")
        self.assertEqual(opener.calls, 2)

    def test_missing_done_is_flagged_but_text_is_kept(self):
        opener = FakeOpener([FakeResponse([chunk_for("trunc").encode("utf-8")])])
        result = stream_completion("http://x/v1/chat/completions", {"stream": True},
                                   retry_policy=self.policy(), sleep=lambda s: None, opener=opener)
        self.assertEqual(result.text, "trunc")
        self.assertIn("[DONE]", result.error)


class _Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        return

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        self.rfile.read(length)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        for piece in ("Hel", "lo", " SSE"):
            self.wfile.write(chunk_for(piece).encode("utf-8"))
            self.wfile.flush()
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()


class TestRealSocket(unittest.TestCase):
    def test_streams_over_a_real_socket(self):
        server = HTTPServer(("127.0.0.1", 0), _Handler)
        thread = threading.Thread(target=server.serve_forever)
        thread.daemon = True
        thread.start()
        try:
            url = "http://127.0.0.1:%d/v1/chat/completions" % server.server_address[1]
            seen = []
            result = stream_completion(url, {"stream": True}, on_delta=seen.append,
                                       retry_policy={"attempts": 1}, timeout=5)
            self.assertEqual(result.text, "Hello SSE")
            self.assertEqual("".join(seen), "Hello SSE")
            self.assertEqual(result.error, "")
        finally:
            server.shutdown()
            server.server_close()


class Read1Response(FakeResponse):
    """A response that only offers read1(); read() must never be called on it."""

    def read1(self, n=4096):
        if self._chunks:
            return self._chunks.pop(0)
        return b""

    def read(self, n=4096):
        raise AssertionError("read() buffers until n bytes or EOF and defeats streaming")


class QueueResponse(object):
    """Hands out chunks as a producer thread delivers them."""

    def __init__(self):
        self.q = queue.Queue()

    def read1(self, n=4096):
        item = self.q.get(timeout=10)
        return b"" if item is None else item

    def read(self, n=4096):
        raise AssertionError("read() buffers until n bytes or EOF and defeats streaming")

    def close(self):
        return


class TestStreamingIsIncremental(unittest.TestCase):
    def test_read1_is_preferred_over_read(self):
        text, deltas, saw_done = read_stream(Read1Response([BODY.encode("utf-8")]), None)
        self.assertEqual(text, "Hello SSE")
        self.assertTrue(saw_done)

    def test_each_delta_is_delivered_before_the_stream_ends(self):
        # Ordering, not timing: the second chunk is only produced once the first
        # delta has been handed to the caller. A buffering reader cannot pass this.
        response = QueueResponse()
        marks = {}
        seen = []

        def on_delta(text):
            seen.append(text)
            marks.setdefault(text, time.monotonic())
            if text == "one":
                gate.set()

        gate = threading.Event()

        def producer():
            response.q.put(chunk_for("one").encode("utf-8"))
            gate.wait(5)                       # only emit the rest after delta one landed
            marks["rest_sent"] = time.monotonic()
            response.q.put(chunk_for("two").encode("utf-8"))
            response.q.put(b"data: [DONE]\n\n")
            response.q.put(None)

        thread = threading.Thread(target=producer)
        thread.daemon = True
        thread.start()
        text, deltas, saw_done = read_stream(response, on_delta)
        thread.join(timeout=10)
        self.assertIn("rest_sent", marks, "the producer never got past the first delta")
        self.assertLess(marks["one"], marks["rest_sent"],
                        "delta one must reach the caller while the stream is still open")
        self.assertEqual(text, "onetwo")
        self.assertEqual(seen, ["one", "two"])
        self.assertTrue(saw_done)


if __name__ == "__main__":
    unittest.main(verbosity=2)
