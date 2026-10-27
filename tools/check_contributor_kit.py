#!/usr/bin/env python3
"""Contributor kit check: every GitHub issue form and the label list parse as valid YAML and follow the issue-form schema, every label the forms use
is in .github/labels.yml, the pull request template and CONTRIBUTING.md carry the legal rules, and the forms' doc references exist.
No network, no game data. Needs PyYAML.   usage: tools/check_contributor_kit.py [--root <Napple dir>]   exit 0 = ok."""
import argparse
import re
import sys
from pathlib import Path

try:
    import yaml
except ImportError:
    sys.exit("check_contributor_kit.py needs PyYAML (pip install pyyaml)")

BODY_TYPES = {"markdown", "textarea", "input", "dropdown", "checkboxes"}
FIELD_TYPES = BODY_TYPES - {"markdown"}
REQUIRED_FORMS = {"bug_report", "compat-report", "graphics-bug", "mod-fix", "feature_request", "catalogue-doc"}


def check_form(path, labels, root, bad):
    name = path.name
    try:
        form = yaml.safe_load(path.read_text())
    except yaml.YAMLError as e:
        bad.append(f"{name}: invalid YAML: {e}")
        return
    if not isinstance(form, dict):
        bad.append(f"{name}: not a mapping")
        return
    for k in ("name", "description", "body"):
        if not form.get(k):
            bad.append(f"{name}: missing '{k}'")
    for lab in form.get("labels", []) or []:
        if lab not in labels:
            bad.append(f"{name}: label '{lab}' is not in .github/labels.yml")
    ids = set()
    fields = 0
    for i, item in enumerate(form.get("body") or []):
        t = item.get("type") if isinstance(item, dict) else None
        where = f"{name}: body[{i}]"
        if t not in BODY_TYPES:
            bad.append(f"{where}: unknown type {t!r}")
            continue
        attrs = item.get("attributes") or {}
        if t == "markdown":
            if not attrs.get("value"):
                bad.append(f"{where}: markdown needs attributes.value")
            continue
        fields += 1
        if not attrs.get("label"):
            bad.append(f"{where}: {t} needs attributes.label")
        fid = item.get("id")
        if fid is not None:
            if not re.fullmatch(r"[A-Za-z0-9_-]+", str(fid)):
                bad.append(f"{where}: bad id {fid!r}")
            if fid in ids:
                bad.append(f"{where}: duplicate id {fid!r}")
            ids.add(fid)
        if t == "dropdown":
            opts = attrs.get("options")
            if not opts or not all(isinstance(o, str) for o in opts) or len(set(opts)) != len(opts):
                bad.append(f"{where}: dropdown needs unique string options")
        if t == "checkboxes":
            opts = attrs.get("options")
            if not opts or not all(isinstance(o, dict) and o.get("label") for o in opts):
                bad.append(f"{where}: checkboxes need options with labels")
    if not fields:
        bad.append(f"{name}: no input fields")
    # GitHub forbids required on markdown only; also require the legal reminder in every form
    if not re.search(r"keys|firmware|game", path.read_text(), re.I):
        bad.append(f"{name}: no reminder about keys/firmware/game files")
    for ref in re.findall(r"(docs/[\w./-]+\.md)", path.read_text()):
        if not (root / ref).exists():
            bad.append(f"{name}: references missing file {ref}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(Path(__file__).resolve().parents[1]))
    root = Path(ap.parse_args().root).resolve()
    bad = []
    try:
        label_list = yaml.safe_load((root / ".github/labels.yml").read_text())
        labels = {x["name"] for x in label_list}
        for x in label_list:
            if not re.fullmatch(r"[0-9a-f]{6}", str(x.get("color", ""))):
                bad.append(f"labels.yml: bad color for {x.get('name')}")
    except (OSError, yaml.YAMLError, KeyError, TypeError) as e:
        bad.append(f"labels.yml: {e}")
        labels = set()
    forms = sorted((root / ".github/ISSUE_TEMPLATE").glob("*.yml"))
    names = {p.stem for p in forms}
    for missing in sorted(REQUIRED_FORMS - names):
        bad.append(f"issue form {missing}.yml missing")
    for p in forms:
        if p.name == "config.yml":
            try:
                cfg = yaml.safe_load(p.read_text())
                if cfg.get("blank_issues_enabled") is not False:
                    bad.append("config.yml: blank issues should be disabled")
                for c in cfg.get("contact_links", []):
                    if not all(c.get(k) for k in ("name", "url", "about")):
                        bad.append("config.yml: contact link needs name, url, about")
            except (yaml.YAMLError, AttributeError) as e:
                bad.append(f"config.yml: {e}")
        else:
            check_form(p, labels, root, bad)
    pr = root / ".github/PULL_REQUEST_TEMPLATE.md"
    if not pr.exists():
        bad.append("PULL_REQUEST_TEMPLATE.md missing")
    else:
        text = pr.read_text()
        for needle in ("Signed-off", "git commit -s", "keys", "check_docs"):
            if needle.lower() not in text.lower():
                bad.append(f"PULL_REQUEST_TEMPLATE.md: no mention of '{needle}'")
    c = (root / "CONTRIBUTING.md").read_text()
    for needle in ("## Legal rules first", "## Sign-off (DCO)", "Signed-off-by"):
        if needle not in c:
            bad.append(f"CONTRIBUTING.md: missing '{needle}'")
    if c.find("## Legal rules first") > c.find("## Build"):
        bad.append("CONTRIBUTING.md: legal rules must come before the build section")
    for b in bad:
        print("FAIL", b)
    print(f"checked {len(forms)} issue form files, {len(labels)} labels: {'OK' if not bad else str(len(bad)) + ' problems'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
