#!/usr/bin/env python3
"""Regenerate site/{terms,privacy,report}.html from docs/legal/*.md (small markdown subset)."""
import html, re, pathlib
root = pathlib.Path(__file__).resolve().parents[2]
PAGES = {"terms.md": "terms.html", "privacy.md": "privacy.html", "report-policy.md": "report.html"}

def inline(s):
    s = html.escape(s, quote=False)
    s = re.sub(r"`([^`]+)`", r"<code>\1</code>", s)
    s = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", s)
    s = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", lambda m: '<a href="%s">%s</a>' % (PAGES.get(m.group(2), m.group(2)), m.group(1)), s)
    s = s.replace("(report.html)", "")
    return s

def convert(md):
    out, lines, i, title = [], md.splitlines(), 0, ""
    while i < len(lines):
        l = lines[i]
        if l.startswith("# "): title = l[2:]; out.append("<h1>%s</h1>" % inline(l[2:]))
        elif l.startswith("## "): out.append("<h2>%s</h2>" % inline(l[3:]))
        elif l.startswith("|"):
            rows = []
            while i < len(lines) and lines[i].startswith("|"):
                rows.append([c.strip() for c in lines[i].strip("|").split("|")]); i += 1
            i -= 1
            out.append("<div class=\"tbl\"><table><thead><tr>%s</tr></thead><tbody>%s</tbody></table></div>" % (
                "".join("<th>%s</th>" % inline(c) for c in rows[0]),
                "".join("<tr>%s</tr>" % "".join("<td>%s</td>" % inline(c) for c in r) for r in rows[2:])))
        elif re.match(r"(- |\d+\. )", l):
            tag = "ul" if l.startswith("- ") else "ol"; items = []
            while i < len(lines) and re.match(r"(- |\d+\. )", lines[i]):
                items.append("<li>%s</li>" % inline(re.sub(r"^(- |\d+\. )", "", lines[i]))); i += 1
            i -= 1; out.append("<%s>%s</%s>" % (tag, "".join(items), tag))
        elif l.strip(): out.append("<p>%s</p>" % inline(l))
        i += 1
    return title, "\n".join(out)

TPL = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>%(t)s · Napple</title>
<meta name="description" content="%(t)s for the optional Napple Online service. Version v1, effective 2026-11-03.">
<meta name="theme-color" content="#120c1a">
<link rel="icon" href="assets/favicon.png">
<link rel="stylesheet" href="style.css">
</head>
<body>
<div class="bg" aria-hidden="true"></div>
<header class="nav">
  <a class="brand" href="./"><img src="assets/favicon.png" alt="" width="28" height="28"> Napple</a>
  <nav><a href="./">Home</a><a href="terms.html">Terms</a><a href="privacy.html">Privacy</a><a href="report.html">Reports</a><a href="docs/">Docs</a></nav>
</header>
<main class="legal">
%(b)s
</main>
<footer>
  <div class="f-row">
    <span class="brand"><img src="assets/favicon.png" alt="" width="22" height="22"> Napple</span>
    <span class="links"><a href="terms.html">Terms</a><a href="privacy.html">Privacy</a><a href="report.html">Reports &amp; takedowns</a><a href="./">Home</a></span>
  </div>
  <p>MIT licensed. Napple is not affiliated with Nintendo.</p>
</footer>
</body>
</html>
"""
for src, dst in PAGES.items():
    t, b = convert((root / "docs/legal" / src).read_text())
    (root / "site" / dst).write_text(TPL % {"t": html.escape(t), "b": b})
