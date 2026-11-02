# Integrator note: claude/online-support-table-2026-11-02-r10

Base: `main` at `d0d4be9`. One commit. Docs, data, generator and landing page only; no runtime, Rust or Swift code.

## What it adds
- `compat/online.json`: the data (schema in `tools/support/gen_online.py`). Later sessions edit only this file, then run
  `python3 -I tools/support/gen_support.py`.
- `tools/support/gen_online.py` (new) + `tools/support/gen_support.py` (calls it): generate `docs/reference/online-support.md`, the README block
  between `<!-- online:begin -->` / `<!-- online:end -->` (Status section, after the per-title table) and `site/online.json`.
  `--check` covers all three, so `tools/check_docs.py` fails on a stale block.
- `site/index.html`, `site/app.js`, `site/style.css`: `#online` section + nav link, rendered from `online.json`.
- `tools/support/test_gen_support.py`: 4 new tests (9 total, all pass).
- Change note `docs/archive/changes/2026-11-02-claude-online-support-table-2026-11-02-r10.md`.

## Outside the listed files (needed for check_docs)
- `docs/archive/changes/README.md`: a new `### Round 2026-11-02-r10` table with one row. The orphan check requires every change note to be
  linked from this index. Other r10 branches will probably add the same heading: on conflict, keep one heading and all rows.
- `docs/reference/support-matrix.md` is regenerated and now links `online-support.md` (that keeps the page reachable from `docs/index.md`).

## Merge notes
- After merging anything that touches README.md, `compat/matrix.json`, `docs/titles/` or `compat/online.json`, run
  `python3 -I tools/support/gen_support.py` and commit the outputs; then `python3 tools/check_docs.py`.
- If another branch rewrites the README Status section, keep the marker pair once (the generator exits when there are 0 or 2+ blocks).
- Optional follow-ups not done (files outside the allowed list): a row for `reference/online-support.md` in the docs/index.md reference table and
  in the `nav:` of `docs-site/mkdocs.yml` (mkdocs only logs INFO for pages outside the nav, so `--strict` passes without it).

## Decisions to confirm
- SMO is recorded as backend `nex` with rankings / DataStore `untested` (Luigi's Balloon World is online), all multiplayer modes `unsupported`.
  This is from knowledge of the game, not checked on Napple; switch to `none` if Napple does not intend to serve it.
- ACNH: rankings `unsupported` (the game has none), `lan_play` `untested` (a LAN mode is not confirmed).
- `unsupported` = the game lacks the mode; shown as `-` in the README and summary tables.

## Verified (Linux)
`python3 -I tools/support/gen_support.py --check` OK, `python3 tools/check_docs.py` OK (210 files), `python3 -I tools/support/test_gen_support.py`
9 OK. Landing page checked in headless Chromium at 1280 and 390 px: 4 cards, no horizontal scroll, no console errors (served locally).
Not verified: the GitHub Pages / mkdocs build itself (mkdocs not installed here).
