#!/usr/bin/env python3
"""Docs check: every relative link and #anchor in the Markdown files resolves (GitHub heading anchors), docs/symptoms.md rows have 4 cells,
and every Testing Material title has docs/titles/<id>.md. No game files, no network. Idea after nxnative dm/tools/tests/test_docs.py (MIT, seedds);
written fresh for this repository layout.   usage: tools/check_docs.py [--root <Napple dir>]   exit 0 = ok."""
import argparse
import re
import sys
from pathlib import Path
from urllib.parse import unquote

TITLES = ["01006BB00C6F0000", "0100000000010000", "01006F8002326000", "0100152000022000"]
SKIP_DIRS = {"target", "node_modules", ".build", "build", "dist", "site", "third_party", ".git"}


def slug(heading):
    s = re.sub(r"`([^`]*)`", r"\1", heading)
    s = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", s).strip().lower()
    s = re.sub(r"[^\w\- ]", "", s)
    return s.replace(" ", "-")


def anchors(path, cache={}):
    if path not in cache:
        seen, out, fence = {}, set(), False
        for line in path.read_text(errors="replace").split("\n"):
            if line.startswith("```"):
                fence = not fence
            if fence:
                continue
            m = re.match(r"#{1,6}\s+(.*?)\s*#*$", line)
            if m:
                s = slug(m.group(1)); n = seen.get(s, 0); seen[s] = n + 1
                out.add(s if n == 0 else f"{s}-{n}")
        cache[path] = out
    return cache[path]


def links(path):
    text = re.sub(r"```.*?```", lambda m: "\n" * m.group(0).count("\n"), path.read_text(errors="replace"), flags=re.S)
    for n, line in enumerate(text.split("\n"), 1):
        line = re.sub(r"`[^`]*`", "", line)
        for m in re.finditer(r"\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)", line):
            yield n, m.group(1).strip("<>")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(Path(__file__).resolve().parents[1]))
    root = Path(ap.parse_args().root).resolve()
    files = [p for p in list(root.glob("*.md")) + list((root / "docs").rglob("*.md"))
             if not SKIP_DIRS & set(p.relative_to(root).parts)]
    bad = []
    for p in sorted(files):
        for n, t in links(p):
            if re.match(r"[a-z][a-z0-9+.-]*:", t, re.I):
                continue
            ref, _, frag = t.partition("#")
            dest = p if not ref else (p.parent / unquote(ref)).resolve()
            if not dest.exists():
                bad.append(f"{p.relative_to(root)}:{n}: missing file {t}")
            elif frag and dest.suffix == ".md" and unquote(frag).lower() not in anchors(dest):
                bad.append(f"{p.relative_to(root)}:{n}: missing anchor {t}")
    sym = root / "docs/symptoms.md"
    if sym.exists():
        rows = 0
        for n, line in enumerate(sym.read_text().split("\n"), 1):
            if line.startswith("|") and not re.match(r"\|[\s\-|]+\|$", line) and not line.startswith("| Symptom"):
                cells = re.split(r"(?<!\\)\|", line.strip())[1:-1]
                rows += 1
                if len(cells) != 4 or not all(c.strip() for c in cells):
                    bad.append(f"docs/symptoms.md:{n}: row needs 4 non-empty cells (Symptom, Cause, Fix, Found on), has {len(cells)}")
        if rows < 10:
            bad.append(f"docs/symptoms.md: only {rows} rows")
    else:
        bad.append("docs/symptoms.md missing")
    for t in TITLES:
        if not (root / f"docs/titles/{t}.md").exists():
            bad.append(f"docs/titles/{t}.md missing")
    for b in bad:
        print("FAIL", b)
    print(f"checked {len(files)} Markdown files: {'OK' if not bad else str(len(bad)) + ' problems'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
