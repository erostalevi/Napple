"""mkdocs hooks for the Napple documentation.

The docs are written to read well on GitHub, so they link to repository files outside docs/ (README, COMPATIBILITY.md, knownSignatures/, ...)
and the archived change notes keep links to paths that no longer exist. For the site:
  * a relative link that leaves docs/ becomes a link to the file in the GitHub repository (DOCS_REPO_URL, else https://github.com/$GITHUB_REPOSITORY);
  * in an archived change note, a relative link to a file that does not exist is shown as plain text (the note is history and stays unchanged on disk).
Anything else is left alone so mkdocs --strict still reports real broken links.
"""
import os
import posixpath
import re

LINK = re.compile(r"(\]\()([^)\s]+)((?:\s+\"[^\"]*\")?\))")
FENCE = re.compile(r"(^```.*?^```)", re.S | re.M)


def _repo_blob():
    url = os.environ.get("DOCS_REPO_URL")
    if not url:
        slug = os.environ.get("GITHUB_REPOSITORY", "erostalevi/Napple")
        url = f"https://github.com/{slug}"
    return url.rstrip("/") + "/blob/main/"


def on_page_markdown(markdown, page, config, files):
    src = page.file.src_uri
    history = src.startswith("archive/changes/") and src != "archive/changes/README.md"
    docs_dir = config["docs_dir"]
    root = os.path.dirname(docs_dir)  # the Napple/ directory
    base = _repo_blob()

    def fix(m):
        target = m.group(2)
        if re.match(r"[a-z][a-z0-9+.-]*:|#", target, re.I):
            return m.group(0)
        path, _, frag = target.partition("#")
        if not path:
            return m.group(0)
        norm = posixpath.normpath(posixpath.join(posixpath.dirname(src), path))
        if norm.startswith(".."):
            repo_path = posixpath.normpath(posixpath.join("docs", norm))
            if repo_path.startswith(".."):
                return m.group(0)
            return f"{m.group(1)}{base}{repo_path}{'#' + frag if frag else ''}{m.group(3)}"
        if history and not os.path.exists(os.path.join(docs_dir, norm)):
            return "\x00DEAD\x00" + m.group(0)
        return m.group(0)

    out = []
    for i, chunk in enumerate(FENCE.split(markdown)):
        if i % 2:
            out.append(chunk)
            continue
        chunk = LINK.sub(fix, chunk)
        # "[text](dead)" -> "text" for the links marked dead above
        chunk = re.sub(r"\[([^\]\x00]*)\x00DEAD\x00\]\([^)]*\)", r"\1", chunk)
        out.append(chunk)
    return "".join(out)
