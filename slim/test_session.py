"""Tests for the session store.

The format is shared with the C++ client, so these tests also pin the exact line shape
that the two writers must agree on.
"""
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from session import (MAX_LINE_BYTES, session_append, session_load,   # noqa: E402
                     session_path, valid_session_name)


class TestSessionName(unittest.TestCase):
    def test_accepts(self):
        for name in ("default", "2026-09-18", "a.b_c-1", "A" * 64):
            ok, why = valid_session_name(name)
            self.assertTrue(ok, "%s should be valid (%s)" % (name, why))

    def test_rejects(self):
        for name in ("", ".hidden", "a/b", "..", "../etc/passwd", "a" * 65, "a b", "a\nb",
                     "sessions/x", "a\\b"):
            ok, why = valid_session_name(name)
            self.assertFalse(ok, "%r should be rejected" % name)
            self.assertTrue(why)


class TestSessionPath(unittest.TestCase):
    def test_join(self):
        self.assertEqual(session_path("/data", "default"), "/data/sessions/default.jsonl")
        self.assertEqual(session_path("/data/", "default"), "/data/sessions/default.jsonl")


class TestSessionStore(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="slim-session-test-")
        self.path = session_path(self.dir, "default")

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_append_creates_the_directory(self):
        ok, why = session_append(self.path, "user", "hello")
        self.assertTrue(ok, why)
        self.assertTrue(os.path.isdir(os.path.join(self.dir, "sessions")))

    def test_line_shape_matches_the_cpp_writer(self):
        session_append(self.path, "user", "hi")
        with open(self.path, "rb") as fh:
            line = fh.readline().decode("utf-8")
        # One compact JSON object per line: no spaces after ':' or ',', raw UTF-8.
        self.assertRegex(line, r'^\{"ts":"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z","role":"user","text":"hi"\}\n$')

    def test_non_ascii_is_stored_raw_not_escaped(self):
        session_append(self.path, "user", "溫度 25.5°C")
        with open(self.path, "rb") as fh:
            raw = fh.read()
        self.assertIn("溫度".encode("utf-8"), raw, "text must stay raw UTF-8 for the C++ reader")
        self.assertNotIn(b"\\u", raw)

    def test_round_trip_with_awkward_text(self):
        text = 'multi\nline "quoted" back\\slash\ttab'
        session_append(self.path, "user", text)
        session_append(self.path, "assistant", "ok")
        turns = session_load(self.path)
        self.assertEqual([t["role"] for t in turns], ["user", "assistant"])
        self.assertEqual(turns[0]["text"], text)

    def test_limit_returns_the_newest_turns_oldest_first(self):
        for i in range(5):
            session_append(self.path, "user", "turn %d" % i)
        turns = session_load(self.path, limit=2)
        self.assertEqual([t["text"] for t in turns], ["turn 3", "turn 4"])

    def test_zero_means_none_and_negative_means_all(self):
        for i in range(3):
            session_append(self.path, "user", "t%d" % i)
        self.assertEqual(session_load(self.path, 0), [])
        self.assertEqual(len(session_load(self.path, -1)), 3)
        self.assertEqual(len(session_load(self.path)), 3, "the default is every turn")

    def test_missing_file_is_an_empty_session(self):
        self.assertEqual(session_load(session_path(self.dir, "never-used")), [])

    def test_broken_lines_are_skipped(self):
        session_append(self.path, "assistant", "good one")
        with open(self.path, "ab") as fh:
            fh.write(b'{"ts":"x","role":"user"\n')          # truncated
            fh.write(b'not json at all\n')                   # garbage
            fh.write(b'{"role":1,"text":"wrong types"}\n')   # wrong types
            fh.write(b'["a","list"]\n')                      # not an object
            fh.write(b'\n')                                  # empty
        session_append(self.path, "user", "good two")
        turns = session_load(self.path)
        self.assertEqual([t["text"] for t in turns], ["good one", "good two"])

    def test_oversized_line_is_skipped(self):
        session_append(self.path, "assistant", "short")
        with open(self.path, "ab") as fh:
            fh.write(b'{"ts":"x","role":"user","text":"' + b"a" * MAX_LINE_BYTES + b'"}\n')
        turns = session_load(self.path)
        self.assertEqual([t["text"] for t in turns], ["short"])

    def test_unreadable_file_is_an_empty_session_not_a_crash(self):
        self.assertEqual(session_load(self.dir), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
