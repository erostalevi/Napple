#!/usr/bin/env python3
"""Site outputs of the support generator (offline, stdlib only). Called by gen_support.py, which writes and --checks them with the other tables.

  site/compat.json    per-title status (macOS stages, state, performance, known issues, iOS) and the online table, for site/compat.html
                      (site/compat.js renders it client-side)
  site/compat.html    only the static fallback between <!-- compat:begin --> and <!-- compat:end -->: the same lists as plain tables,
                      shown when JavaScript or the JSON fetch is unavailable
  site/index.html     only the coverage block between <!-- coverage:begin --> and <!-- coverage:end -->, from the coverage snapshot,
                      so the landing page numbers always match docs/reference/feature-support.md and the snapshot

check_links(root) lists relative links in site/*.html that do not resolve (files and #anchors); docs/ is skipped (mkdocs builds it at deploy).
A missing site page (scratch roots in the tests) is skipped, like README.md in gen_online.py.
"""
import html
import json
import re
import sys
from pathlib import Path

COMPAT_JSON = "site/compat.json"
COMPAT_HTML = "site/compat.html"
INDEX_HTML = "site/index.html"
COMPAT_MARK = ("<!-- compat:begin -->", "<!-- compat:end -->")
COVERAGE_MARK = ("<!-- coverage:begin -->", "<!-- coverage:end -->")
STAGES = [("boot", "Boot"), ("graphics", "Graphics"), ("audio", "Audio"), ("input", "Input"), ("saves", "Saves")]
STAGE_STATUSES = {
    "pass": "the stage passed in a scripted or played run",
    "partial": "the stage works in part; the issues say what is missing",
    "fail": "the stage was run and fails",
    "blocked": "an earlier stage stops the run before this one",
    "untested": "not run yet",
    "n/t": "the title is not in the scripted matrix; read its note",
}
# coverage rows on the landing page: snapshot domain key -> label (all snapshot domains, in snapshot order)
COVERAGE_LABELS = {
    "cpu_rewrite": "CPU · AOT A64 rewrite", "loader": "Loader · NSO / NRO / relocations", "kernel": "Kernel · svcs + scheduling",
    "ipc": "IPC · HLE service coverage", "ipc_depth": "IPC · command depth", "nvn": "GPU · NVN API surface",
    "shader": "GPU · Maxwell shader ISA", "gpu_fixed": "GPU · formats + pipeline", "nvdrv": "GPU · nvdrv / host1x devices",
    "audio": "Audio", "input": "Input", "content": "Content · containers, crypto, DLC", "fs_runtime": "Saves + storage",
    "applets": "Library applets", "media": "Video / media decode",
}
E = html.escape


def note_url(tid):
    return f"docs/titles/{tid}/"  # mkdocs use_directory_urls: docs/titles/<id>.md is served at /docs/titles/<id>/


def compat_data(last_run, titles, snap, online):
    return {"schema": 1, "generator": "tools/support/gen_support.py (module gen_site.py)",
            "sources": ["compat/matrix.json", "docs/titles/*.md", "compat/online.json"],
            "matrix_last_run": last_run, "platform": "macOS on Apple Silicon (iOS deferred)",
            "stages": [{"key": k, "label": label} for k, label in STAGES], "stage_statuses": STAGE_STATUSES,
            "titles": [{"title_id": t["title_id"], "name": t["name"], "version": t["version"], "in_matrix": t["in_matrix"],
                        "state": t["macos"]["state"], "stages": {k: t["macos"][k] or "n/t" for k, _ in STAGES},
                        "performance": t["performance"], "known_issues": t["known_issues"], "ios": t["ios"],
                        "note": note_url(t["title_id"]) if t["note"] else None} for t in sorted(titles, key=lambda t: t["name"].lower())],
            "online": online}


def render_compat_block(data):
    stage_head = "".join(f"<th>{E(label)}</th>" for _, label in STAGES)
    L = [COMPAT_MARK[0],
         '<div class="fallback" id="compat-fallback">',
         f'<p class="fine">Static copy of <a href="compat.json">compat.json</a>. Matrix last run {E(data["matrix_last_run"] or "never")}; '
         f'online statuses last checked {E(data["online"]["last_checked"])}.</p>',
         '<h3>Titles (macOS)</h3>',
         f'<div class="tbl"><table><thead><tr><th>Title</th><th>Version</th>{stage_head}<th>State</th></tr></thead><tbody>']
    for t in data["titles"]:
        name = f'<a href="{E(t["note"])}">{E(t["name"])}</a>' if t["note"] else E(t["name"])
        cells = "".join(f'<td>{E(t["stages"][k])}</td>' for k, _ in STAGES)
        L.append(f'<tr><td>{name}<br><code>{E(t["title_id"])}</code></td><td>{E(t["version"] or "")}</td>{cells}<td>{E(t["state"])}</td></tr>')
    L.append("</tbody></table></div>")
    on = data["online"]
    mode_head = "".join(f'<th>{E(m["short"])}</th>' for m in on["modes"])
    L += ['<h3>Online (Napple to Napple)</h3>',
          f'<div class="tbl"><table><thead><tr><th>Title</th><th>Backend</th>{mode_head}</tr></thead><tbody>']
    for t in on["titles"]:
        cells = "".join(f'<td>{"-" if t["modes"][m["key"]]["status"] == "unsupported" else E(t["modes"][m["key"]]["status"])}</td>'
                        for m in on["modes"])
        L.append(f'<tr><td>{E(t["name"])}</td><td>{E(t["backend"])}</td>{cells}</tr>')
    L += ["</tbody></table></div>", '<p class="fine"><code>-</code> means the game has no such mode.</p>', "</div>", COMPAT_MARK[1]]
    return "\n".join(L)


def render_coverage_block(snap):
    L = [COVERAGE_MARK[0], '<div class="cov" id="cov">']
    for d in snap["domains"]:
        pct = round(d["judged_ratio"] * 100)
        label = COVERAGE_LABELS.get(d["key"], d["title"])
        L.append(f'  <div class="row"><span>{E(label)}</span><i style="--v:{pct}"></i><b>{pct}%</b></div>')
    L += [f'  <div class="row total"><span>Overall platform estimate</span><i style="--v:{round(snap["final_percent"])}"></i>'
          f'<b>≈ {snap["final_percent"]}%</b></div>', "</div>",
          f'<p class="fine">%Analysis snapshot {E(snap["date"])}, commit <code>{E(snap["commit"])}</code>. '
          "An estimate of how much of the Switch platform surface the pipeline and runtime implement, not a per-game compatibility score.</p>",
          COVERAGE_MARK[1]]
    return "\n".join(L)


def splice(path_label, text, marks, block):
    b, e = marks
    i, j = text.find(b), text.find(e)
    if i < 0 or j < i or text.count(b) != 1 or text.count(e) != 1:
        sys.exit(f"{path_label}: needs exactly one {b} ... {e} block")
    return text[:i] + block + text[j + len(e):]


def build(root, last_run, titles, snap, online):
    data = compat_data(last_run, titles, snap, online)
    outs = {COMPAT_JSON: json.dumps(data, indent=1, sort_keys=True, ensure_ascii=False) + "\n"}
    for rel, marks, block in [(COMPAT_HTML, COMPAT_MARK, render_compat_block(data)), (INDEX_HTML, COVERAGE_MARK, render_coverage_block(snap))]:
        p = root / rel
        if p.exists():
            outs[rel] = splice(rel, p.read_text(), marks, block)
    return outs


def check_links(root, pages=None):
    """Relative href/src in site/*.html must resolve to a file in site/ (and a #fragment to an id in that page). docs/ is built at deploy."""
    site = root / "site"
    bad = []
    ids = {}

    def ids_of(p):
        if p not in ids:
            ids[p] = set(re.findall(r'\bid="([^"]+)"', p.read_text()))
        return ids[p]

    for page in sorted(site.glob("*.html")) if pages is None else pages:
        text = page.read_text()
        for n, line in enumerate(text.split("\n"), 1):
            for m in re.finditer(r'\b(?:href|src)="([^"]*)"', line):
                t = m.group(1)
                if not t or re.match(r"[a-z][a-z0-9+.-]*:", t, re.I) or t.startswith("//"):
                    continue
                ref, _, frag = t.partition("#")
                ref = ref.split("?")[0]
                if ref == "docs" or ref.startswith("docs/"):
                    continue
                dest = page if not ref else (page.parent / ref)
                if dest.is_dir():
                    dest = dest / "index.html"
                where = f"site/{page.name}:{n}"
                if not dest.exists():
                    bad.append(f"{where}: missing file {t}")
                elif frag and dest.suffix == ".html" and frag not in ids_of(dest):
                    bad.append(f"{where}: missing anchor {t}")
    return bad
