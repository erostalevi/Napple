#!/usr/bin/env python3
"""Tests for gen_support.py: python3 -I tools/support/test_gen_support.py   (stdlib unittest, offline)."""
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
GEN = HERE / "gen_support.py"
INPUTS = ["compat/matrix.json", "compat/online.json", "docs/titles", "tools/support/data/coverage-snapshot.json", "README.md",
          "site"]


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

    def test_online_outputs_written_and_checked(self):
        self.assertEqual(run(self.tmp).returncode, 0)
        for rel in ["docs/reference/online-support.md", "site/online.json"]:
            self.assertTrue((self.tmp / rel).exists(), rel)
        readme = self.tmp / "README.md"
        readme.write_text(readme.read_text().replace("| works |", "| broken |", 1))
        r = run(self.tmp, "--check")
        self.assertEqual(r.returncode, 1)
        self.assertIn("README.md", r.stdout)
        self.assertEqual(run(self.tmp).returncode, 0)
        self.assertEqual(run(self.tmp, "--check").returncode, 0)

    def test_online_data_change_makes_outputs_stale(self):
        run(self.tmp)
        data = self.tmp / "compat/online.json"
        data.write_text(data.read_text().replace('"status": "broken"', '"status": "works"', 1))
        r = run(self.tmp, "--check")
        self.assertEqual(r.returncode, 1)
        for rel in ["online-support.md", "README.md", "site/online.json"]:
            self.assertIn(rel, r.stdout)

    def test_online_invalid_status_is_rejected(self):
        data = self.tmp / "compat/online.json"
        data.write_text(data.read_text().replace('"status": "broken"', '"status": "kinda"', 1))
        r = run(self.tmp)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("kinda", r.stderr)

    def test_readme_without_markers_is_rejected(self):
        readme = self.tmp / "README.md"
        readme.write_text(readme.read_text().replace("<!-- online:begin -->", "", 1))
        r = run(self.tmp, "--check")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("online:begin", r.stderr)

    def test_compat_json_and_site_blocks(self):
        self.assertEqual(run(self.tmp).returncode, 0)
        data = json.loads((self.tmp / "site/compat.json").read_text())
        self.assertEqual({t["title_id"] for t in data["titles"]}, {p.stem for p in (self.tmp / "docs/titles").glob("0*.md")})
        self.assertTrue(data["online"]["titles"])
        page = (self.tmp / "site/compat.html").read_text()
        for t in data["titles"]:
            self.assertIn(t["title_id"], page)  # the static fallback lists every title
        snap = json.loads((self.tmp / "tools/support/data/coverage-snapshot.json").read_text())
        self.assertIn(f"≈ {snap['final_percent']}%", (self.tmp / "site/index.html").read_text())
        idx = self.tmp / "site/index.html"
        idx.write_text(idx.read_text().replace("<b>98%</b>", "<b>50%</b>", 1))
        r = run(self.tmp, "--check")
        self.assertEqual(r.returncode, 1)
        self.assertIn("site/index.html", r.stdout)

    def test_compat_html_without_markers_is_rejected(self):
        page = self.tmp / "site/compat.html"
        page.write_text(page.read_text().replace("<!-- compat:begin -->", "", 1))
        r = run(self.tmp)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("compat:begin", r.stderr)

    def test_broken_site_link_fails_check(self):
        run(self.tmp)
        self.assertEqual(run(self.tmp, "--check").returncode, 0)
        (self.tmp / "site/compat.js").unlink()
        self.assertIn("missing file compat.js", run(self.tmp, "--check").stdout)
        page = self.tmp / "site/compat.html"
        page.write_text(page.read_text().replace('href="#titles"', 'href="#no-such-anchor"', 1))
        self.assertIn("missing anchor #no-such-anchor", run(self.tmp, "--check").stdout)

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
