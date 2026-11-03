# 2026-10-26 support-tables (cloud2): generated per-title and per-platform support tables

Branch `claude/support-tables-2026-10-26-cloud2`, Linux cloud. Roadmap step 3 (catalogue and support tables), prepared early.

## What changed
- `tools/support/gen_support.py` (python3 stdlib, offline, run with `-I`) writes `docs/reference/support-matrix.md` (per title: boot, graphics, audio, input, saves from `compat/matrix.json`; performance and known issues from the title notes and matrix reasons; iOS column from the table in `docs/titles/README.md`), `docs/reference/feature-support.md` (domain coverage, impl/partial/stub/missing counts, the not-implemented list, ranked next items) and `docs/reference/support-data.json` (same data for the site).
- `--check` writes nothing and fails naming each stale file. `tools/check_docs.py` runs it (appended block), and the `docs` CI job runs the generator test.
- `tools/support/test_gen_support.py` (5 tests): check fails when files are missing, after a hand edit, and after a title-note change; committed tables are current; every title note is listed.
- `tools/support/data/coverage-snapshot.json` is a trimmed copy of the newest percent-analysis snapshot (2026-10-26, 332988e) so the generator works in a Napple-only checkout. `--refresh-snapshot` re-imports the newest one from `.claude/skills/percent-analysis/history`.
- Linked from `docs/index.md`, `tools/README.md`, `docs/guides/testing.md`.

## Decisions
- The site (`site/`) is a static landing page with no data file; the data hook is the docs-site: mkdocs copies `support-data.json` to `/docs/reference/support-data.json`. No site code changed.
- Only two titles are in `compat/matrix.json`; MK8D and ACNH show `n/t` (not tracked) in the stage columns rather than invented statuses; their state comes from the notes. The matrix is read only.
- The snapshot is vendored (not read from `.claude/`) so `--check` is deterministic anywhere; refresh it after each percent-analysis run, then regenerate.

## Verified
- `python3 -I tools/support/test_gen_support.py`: 5 tests OK. `python3 tools/check_docs.py`: OK (and fails with a stale table, shown by hand).
- `mkdocs build --strict -f docs-site/mkdocs.yml` (mkdocs 1.6.1 + material 9.5.50, installed here): builds; `support-data.json` is in the output. Not deployed.
- Not verified: the GitHub Pages deploy itself; the Mac (nothing here touches product code).

## Files
`tools/support/{gen_support.py,test_gen_support.py,data/coverage-snapshot.json}`, `docs/reference/{support-matrix.md,feature-support.md,support-data.json}`, `tools/check_docs.py`, `.github/workflows/tests.yml`, `docs/index.md`, `tools/README.md`, `docs/guides/testing.md`.
