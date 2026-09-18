#!/usr/bin/env python3
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import rag


class RagTests(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.mkdtemp()
        self.db = os.path.join(self.td, "t.sqlite")
        self.docs = os.path.join(self.td, "docs")
        os.makedirs(self.docs)
        with open(os.path.join(self.docs, "note.md"), "w", encoding="utf-8") as fh:
            fh.write("T830 is an OpenWrt CPE with musl aarch64.\n")
        self.conn = rag.connect(self.db)
        rag.ingest_tree(self.conn, self.docs, prefix="doc")

    def tearDown(self):
        self.conn.close()
        shutil.rmtree(self.td)

    def test_search_hits(self):
        hits = rag.search(self.conn, "OpenWrt CPE")
        self.assertTrue(hits)
        self.assertIn("doc/", hits[0]["path"])

    def test_empty_query(self):
        self.assertEqual(rag.search(self.conn, "  "), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
