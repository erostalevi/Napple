#!/usr/bin/env python3
"""Generates the support tables from data that is already in the repository (offline, stdlib only, no game data, no keys, no firmware):

  docs/reference/support-matrix.md    per-title status per platform (macOS now, iOS deferred)
  docs/reference/feature-support.md   feature coverage by domain from the newest coverage snapshot
  docs/reference/support-data.json    the same data for the site (mkdocs copies it to /docs/reference/support-data.json)

Inputs: compat/matrix.json (read only), docs/titles/*.md (state, open items, numbers, iOS column of the table in docs/titles/README.md) and
tools/support/data/coverage-snapshot.json (a trimmed copy of the newest .claude/skills/percent-analysis/history snapshot; refresh it with --refresh-snapshot).
Title names and ids only; the matrix holds metadata, never game bytes.

usage: python3 -I tools/support/gen_support.py [--root <Napple dir>] [--check] [--refresh-snapshot [history dir]]
  (no flag)            write the three files
  --check              write nothing; exit 1 and name the files when one is stale (used by tools/check_docs.py and CI)
  --refresh-snapshot   rebuild tools/support/data/coverage-snapshot.json from the newest snapshot of the percent-analysis history, then write
"""
import argparse
import json
import re
import sys
from pathlib import Path

OUTPUTS = ["docs/reference/support-matrix.md", "docs/reference/feature-support.md", "docs/reference/support-data.json"]
SNAPSHOT = "tools/support/data/coverage-snapshot.json"
HISTORY = "../.claude/skills/percent-analysis/history"
MATRIX = "compat/matrix.json"
ID_RE = re.compile(r"^[0-9A-F]{16}$")
# matrix stage -> table column
COLUMNS = [("boot", "runtime-boot"), ("graphics", "video"), ("audio", "audio"), ("input", "input"), ("saves", "saves")]
STATUS_WORD = {"pass": "pass", "partial": "partial", "fail": "fail", "blocked": "blocked", "untested": "untested"}
STATUS_MARK = {"pass": "OK", "partial": "~", "fail": "X", "blocked": "B", "untested": ".", None: "n/t"}
ITEM_ORDER = ["impl", "partial", "stub", "missing"]


def cell(text):
    return re.sub(r"\s+", " ", str(text)).replace("|", "\\|").strip()


def first_sentence(text, limit=220):
    text = re.sub(r"\s+", " ", re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)).strip()
    m = re.match(r"(.+?[.;])(\s|$)", text)
    out = m.group(1) if m and len(m.group(1)) <= limit else text
    if len(out) > limit:
        out = out[:limit].rsplit(" ", 1)[0] + " ..."
    return out.rstrip(";")


def clip(text, limit):
    text = re.sub(r"\s+", " ", re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)).strip()
    return text if len(text) <= limit else text[:limit].rsplit(" ", 1)[0] + " ..."


def section(text, heading):
    m = re.search(r"^## " + re.escape(heading) + r"\s*\n(.*?)(?=^## |\Z)", text, re.S | re.M)
    return m.group(1) if m else ""


def bullets(block):
    return [re.sub(r"^\s*-\s+", "", ln) for ln in block.split("\n") if re.match(r"\s*- ", ln)]


def read_title_note(path):
    text = path.read_text()
    h1 = re.match(r"# (.+?) \(([0-9A-F]{16})\)", text)
    state = re.search(r"^\*\*State:\*\*\s*(.+)$", text, re.M)
    open_items = [first_sentence(b) for b in bullets(section(text, "What is open"))]
    numbers = bullets(section(text, "Numbers"))
    return {"name": h1.group(1) if h1 else path.stem, "state": clip(state.group(1), 400) if state else "",
            "open": open_items, "performance": first_sentence(numbers[0], 160) if numbers else ""}


def read_ios_column(path):
    out = {}
    for ln in path.read_text().split("\n"):
        cells = [c.strip() for c in ln.strip().strip("|").split("|")]
        if ln.startswith("|") and len(cells) >= 5 and ID_RE.match(cells[1]):
            out[cells[1]] = cells[3]
    return out


def matrix_titles(root):
    data = json.loads((root / MATRIX).read_text())
    out = {}
    for g in data["games"]:
        versions = sorted(g["versions"], key=lambda v: (v["kind"] != "base", v["version"]))
        base = [v for v in versions if v["kind"] == "base"] or versions
        v = base[-1]
        out[g["title_id"]] = {"name": g["name"], "version": v.get("display_version", str(v["version"])), "build_id": v["build_id"],
                              "stages": v["stages"]}
    return data.get("last_run_iso", ""), out


def build_titles(root):
    last_run, from_matrix = matrix_titles(root)
    tdir = root / "docs/titles"
    ios = read_ios_column(tdir / "README.md")
    ids = sorted(set(from_matrix) | {p.stem for p in tdir.glob("*.md") if ID_RE.match(p.stem)})
    titles = []
    for tid in ids:
        note = tdir / f"{tid}.md"
        info = read_title_note(note) if note.exists() else {"name": from_matrix[tid]["name"], "state": "", "open": [], "performance": ""}
        m = from_matrix.get(tid)
        cols = {}
        issues = []
        for col, stage in COLUMNS:
            st = m["stages"].get(stage) if m else None
            cols[col] = st["status"] if st else None
            if st and st["status"] in ("partial", "fail", "blocked") and st.get("reason"):
                issues.append(f"{col}: {first_sentence(st['reason'], 200)}")
        titles.append({"title_id": tid, "name": info["name"], "matrix_name": m["name"] if m else None,
                       "version": m["version"] if m else None, "build_id": m["build_id"] if m else None,
                       "macos": {"state": info["state"], **cols}, "ios": ios.get(tid, "not recorded"),
                       "performance": info["performance"], "known_issues": issues + info["open"],
                       "note": f"docs/titles/{tid}.md" if note.exists() else None, "in_matrix": m is not None})
    return last_run, titles


def load_snapshot(root):
    return json.loads((root / SNAPSHOT).read_text())


def trim_snapshot(snap):
    keep = ["date", "commit", "final_percent", "range", "measured_percent", "schema"]
    out = {k: snap[k] for k in keep if k in snap}
    out["domains"] = [{k: d[k] for k in ("key", "title", "weight", "judged_ratio", "judged_points", "counts", "adjust_reason")} for d in snap["domains"]]
    out["items"] = snap["items"]
    out["priorities"] = [{k: p.get(k) for k in ("rank", "domain", "item", "effort", "importance", "gain_pts", "why", "where")} for p in snap["priorities"]]
    return out


def refresh_snapshot(root, hist):
    hist = (root / HISTORY) if hist is None else Path(hist)
    files = sorted(p for p in hist.glob("*.json") if json.loads(p.read_text()).get("schema") == 2)
    if not files:
        sys.exit(f"no schema-2 snapshot in {hist}")
    newest = max(files, key=lambda p: (json.loads(p.read_text())["saved_at"], p.name))
    out = root / SNAPSHOT
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(trim_snapshot(json.loads(newest.read_text())), indent=1, sort_keys=True) + "\n")
    print(f"snapshot {newest.name} -> {SNAPSHOT}")


def render_matrix(last_run, titles):
    L = ["# Support matrix", "",
         "Per-title status per platform. **Generated** by `tools/support/gen_support.py` from `compat/matrix.json`, the [title notes](../titles/README.md) "
         "and the iOS column of their table; do not edit by hand (`python3 -I tools/support/gen_support.py` rewrites it, `--check` fails when it is stale). "
         "Metadata only: no game bytes, keys or firmware.", "",
         f"Last `napple compat run`: {last_run or 'never'}. Platforms: macOS is the goal now; iOS is deferred (its column is the last Simulator result recorded in the title notes). "
         "Windows and Linux are not goals.", "",
         "Legend (macOS stages, from the matrix): `OK` pass, `~` partial, `X` fail, `B` blocked, `.` untested, `n/t` title not in the matrix (no scripted stage result; read the note).", "",
         "## macOS", "",
         "| Title | Id | Boot | Graphics | Audio | Input | Saves | Performance | Known issues | Note |", "|---|---|:-:|:-:|:-:|:-:|:-:|---|---|---|"]
    for t in titles:
        m = t["macos"]
        issues = "; ".join(t["known_issues"][:3]) or "none recorded"
        if len(t["known_issues"]) > 3:
            issues += f"; +{len(t['known_issues']) - 3} more in the note"
        note = f"[note](../titles/{t['title_id']}.md)" if t["note"] else "none"
        L.append("| " + " | ".join([cell(t["name"]), f"`{t['title_id']}`"] + [STATUS_MARK[m[c]] for c, _ in COLUMNS]
                                     + [cell(t["performance"] or "not recorded"), cell(issues), note]) + " |")
    L += ["", "### State per title (macOS)", ""]
    for t in titles:
        L.append(f"- **{cell(t['name'])}** (`{t['title_id']}`): {t['macos']['state'] or 'no note'}")
    L += ["", "## iOS (deferred)", "", "| Title | Id | Last recorded iOS Simulator result |", "|---|---|---|"]
    for t in titles:
        L.append(f"| {cell(t['name'])} | `{t['title_id']}` | {cell(t['ios'])} |")
    L += ["", "See also [COMPATIBILITY.md](../../COMPATIBILITY.md) (the full matrix with stage evidence), the [coverage by feature](feature-support.md) "
          "and the machine-readable [support-data.json](support-data.json).", ""]
    return "\n".join(L)


def counts_of(items):
    c = {k: 0 for k in ITEM_ORDER}
    for v in items.values():
        c[v] = c.get(v, 0) + 1
    return c


def render_features(snap):
    L = ["# Feature support", "",
         "Coverage of the Switch platform by domain. **Generated** by `tools/support/gen_support.py` from the percent-analysis snapshot "
         f"`{snap['date']}` (commit `{snap['commit']}`, kept in `tools/support/data/coverage-snapshot.json`); do not edit by hand. "
         "The score is an estimate of how much of the platform the pipeline and runtime implement, not per-game compatibility: see the [support matrix](support-matrix.md) for titles. "
         "An item is `impl` when implemented and checked by a test or a run, `partial` or `stub` when it exists but is incomplete, `missing` when absent.", "",
         f"**Overall: {snap['final_percent']}%** (range {snap['range'][0]}-{snap['range'][1]}).", "",
         "## By domain", "", "| Domain | Weight | Coverage | Points | impl | partial | stub | missing |", "|---|--:|--:|--:|--:|--:|--:|--:|"]
    for d in snap["domains"]:
        c = counts_of(snap["items"].get(d["key"], {}))
        L.append(f"| {cell(d['title'])} | {d['weight']} | {round(d['judged_ratio'] * 100)}% | {d['judged_points']:.1f} | "
                 f"{c['impl']} | {c['partial']} | {c['stub']} | {c['missing']} |")
    L += ["", "Domains with no item list (for example IPC depth) are scored from their own counts in the snapshot; the item columns show 0.", "",
          "## What is not fully implemented", ""]
    any_gap = False
    titles = {d["key"]: d["title"] for d in snap["domains"]}
    for key in titles:
        gaps = sorted((n, s) for n, s in snap["items"].get(key, {}).items() if s != "impl")
        if not gaps:
            continue
        any_gap = True
        L += [f"### {cell(titles[key])}", "", "| Item | Status |", "|---|---|"]
        L += [f"| {cell(n)} | {s} |" for n, s in sorted(gaps, key=lambda g: (ITEM_ORDER.index(g[1]) if g[1] in ITEM_ORDER else 9, g[0]))]
        L.append("")
    if not any_gap:
        L += ["Nothing missing in the snapshot.", ""]
    L += ["## Next to implement (ranked by coverage gain per effort)", "", "| # | Domain | Item | Effort | Gain (pts) | Why | Where |", "|--:|---|---|:-:|--:|---|---|"]
    for p in snap["priorities"]:
        L.append(f"| {p['rank']} | {p['domain']} | {cell(p['item'])} | {p['effort']} | {p['gain_pts']} | {cell(p['why'])} | `{p['where']}` |")
    L.append("")
    return "\n".join(L)


def build(root):
    last_run, titles = build_titles(root)
    snap = load_snapshot(root)
    data = {"schema": 1, "generator": "tools/support/gen_support.py", "matrix_last_run": last_run,
            "snapshot": {"date": snap["date"], "commit": snap["commit"]},
            "titles": titles,
            "features": {"final_percent": snap["final_percent"], "range": snap["range"],
                         "domains": [{"key": d["key"], "title": d["title"], "weight": d["weight"], "coverage": d["judged_ratio"],
                                      "points": d["judged_points"], "items": counts_of(snap["items"].get(d["key"], {}))} for d in snap["domains"]],
                         "missing": [{"domain": k, "item": n, "status": s} for k in sorted(snap["items"]) for n, s in sorted(snap["items"][k].items()) if s != "impl"],
                         "priorities": snap["priorities"]}}
    return {OUTPUTS[0]: render_matrix(last_run, titles), OUTPUTS[1]: render_features(snap),
            OUTPUTS[2]: json.dumps(data, indent=1, sort_keys=True) + "\n"}


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(Path(__file__).resolve().parents[2]))
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--refresh-snapshot", nargs="?", const="", default=None, metavar="HISTORY_DIR")
    a = ap.parse_args(argv)
    root = Path(a.root).resolve()
    if a.refresh_snapshot is not None:
        refresh_snapshot(root, a.refresh_snapshot or None)
    outs = build(root)
    if a.check:
        stale = [p for p, t in outs.items() if not (root / p).exists() or (root / p).read_text() != t]
        for p in stale:
            print(f"FAIL {p}: stale or missing; run python3 -I tools/support/gen_support.py")
        print("support tables: " + ("OK" if not stale else f"{len(stale)} stale"))
        return 1 if stale else 0
    for p, t in outs.items():
        (root / p).write_text(t)
        print("wrote", p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
