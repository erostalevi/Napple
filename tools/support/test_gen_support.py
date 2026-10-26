#!/usr/bin/env python3
"""Tests for gen_support.py: python3 -I tools/support/test_gen_support.py   (stdlib unittest, offline)."""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
GEN = HERE / "gen_support.py"
INPUTS = ["compat/matrix.json", "docs/titles", "tools/support/data/coverage-snapshot.json"]


def run(root, *args):
    return subprocess.run([sys.executable, "-I", str(GEN), "--root", str(root), *args], capture_output=True, text=True)


class SupportTables(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        for rel in INPUTS:
            src, dst = ROOT / rel, self.tmp / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(src, dst) if src.is_dir() else shutil.copy(src, dst)
        (self.tmp / "docs/reference").mkdir(parents=True, exist_ok=True)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_check_fails_when_missing_then_passes_after_generate(self):
        self.assertEqual(run(self.tmp, "--check").returncode, 1)
        self.assertEqual(run(self.tmp).returncode, 0)
        self.assertEqual(run(self.tmp, "--check").returncode, 0)

    def test_check_fails_on_stale_file(self):
        run(self.tmp)
        f = self.tmp / "docs/reference/support-matrix.md"
        f.write_text(f.read_text() + "\nhand edit\n")
        r = run(self.tmp, "--check")
        self.assertEqual(r.returncode, 1)
        self.assertIn("support-matrix.md", r.stdout)

    def test_check_fails_when_input_changes(self):
        run(self.tmp)
        note = self.tmp / "docs/titles/01006BB00C6F0000.md"
        note.write_text(note.read_text().replace("**State:** plays", "**State:** crashes", 1))
        self.assertEqual(run(self.tmp, "--check").returncode, 1)

    def test_committed_tables_are_current(self):
        r = run(ROOT, "--check")
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_every_title_note_is_listed_and_no_secrets(self):
        out = (ROOT / "docs/reference/support-matrix.md").read_text()
        for note in (ROOT / "docs/titles").glob("0*.md"):
            self.assertIn(note.stem, out)
        self.assertNotIn("prod.keys", out)


if __name__ == "__main__":
    unittest.main()
