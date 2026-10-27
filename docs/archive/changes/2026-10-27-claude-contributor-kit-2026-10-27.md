# Contributor kit (2026-10-27, cloud, Linux)

Goal: make it easy for strangers (YouTube viewers) to contribute catalogue documentation, mods and fixes, bug reports. Docs, templates and one test script; no product code touched.

## What changed
- `CONTRIBUTING.md`: new **Legal rules first** section (no keys, firmware, game data, dumps, Nintendo code or online code; you own what you send; own the game), **Sign-off (DCO)**, a "Ways to help" table. The existing build/test/PR/style sections are kept; the PR flow now asks for sign-off.
- Issue forms in `.github/ISSUE_TEMPLATE/`: bug report (machine and log capture), compatibility report (how run, frames, exit code), graphics bug, mod or fix submission, feature request, document-a-title (catalogue). `config.yml` turns blank issues off and links Discussions and the private security report. New `.github/PULL_REQUEST_TEMPLATE.md` and `.github/labels.yml` (label proposal).
- Guides: `docs/guides/contributing-{report-a-bug,document-a-title,report-a-graphics-bug,write-a-mod,community}.md`; reference `docs/reference/catalogue-howto.md` (which field feeds `docs/titles/*.md`, `compat/matrix.json`, `gen_support.py`). Wired into `docs/index.md` and `docs/guides/README.md`.
- `tools/check_contributor_kit.py` parses every form and the label list (issue-form schema, labels exist, doc references exist, PR template and CONTRIBUTING sections); `tools/check_docs.py` runs it when PyYAML is installed. Entry in `tools/README.md`.

## Decisions (nobody could be asked)
- **DCO sign-off (`git commit -s`), no CLA**: contributors keep copyright, project stays MIT; no automatic check yet.
- Blank issues disabled so every report arrives through a form; Discussions proposed (categories listed in the community guide); `needs-triage` on every form.
- CODE_OF_CONDUCT.md and SECURITY.md checked: present, consistent, no change. Open items for the maintainer: enable private vulnerability reporting and Discussions in repository settings, create the labels from `.github/labels.yml`, consider a dedicated conduct contact.
- The mod format is not described (mod-framework owns `docs/mods/`): `contributing-write-a-mod.md` has a marked TODO. Same for the golden-frame harness (gfx-correctness): the graphics guide describes only what exists on this base (screenshots, drawlog, `gfx/tests`) with a marked TODO for `tools/golden/README.md`.
- `gen_support.py` outputs unchanged (no title or matrix data was edited), so there was nothing to regenerate; `--check` passes.

## Status
- *tested*: `python3 tools/check_docs.py` (125 files OK), `python3 -I tools/support/gen_support.py --check`, `python3 -I tools/support/test_gen_support.py`, `python3 tools/check_contributor_kit.py` (passes; a deliberately wrong label makes it fail). It found two real YAML errors (unquoted `: ` in descriptions) that are fixed.
- *not verified*: GitHub's own issue-form validator and rendering (needs the repository on GitHub); the commands in the guides were checked against `docs/reference/cli.md` and the existing guides, not run (they need a Mac and a game).
