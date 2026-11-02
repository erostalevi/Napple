# online-support-table (2026-11-02, batch r10): per-game online support table

Branch `claude/online-support-table-2026-11-02-r10` (Linux cloud session, base `main` = `d0d4be9`). Docs and site only; no runtime code.

Napple's online play runs between Napple players only, through the Napple-run servers at `pretend.napple.lat` (napple-online, napple-room),
never Nintendo's. This adds a per-game table of what works there, kept as data so later sessions only edit `compat/online.json`.

## Changes
- `compat/online.json` (new, schema 1): per title id the name, the network backend (`nex`, `npln`, `own-servers`, `none`) and, for each of the
  seven modes (`lan_play`, `local_wireless`, `online_matchmaking`, `online_race_or_match`, `friends`, `rankings`, `datastore`), a status
  (`works`, `partial`, `broken`, `untested`, `unsupported`), a one-line note and the date last checked. The schema is in the docstring of
  `tools/support/gen_online.py`; the generator rejects unknown modes, statuses and backends, missing modes, multi-line notes and bad dates.
- `tools/support/gen_online.py` (new), called by `tools/support/gen_support.py` (loaded by path, because `python3 -I` does not put the script
  directory on `sys.path`). It writes `docs/reference/online-support.md`, the block between `<!-- online:begin -->` and `<!-- online:end -->` in the
  README Status section (nothing else in README is touched; a README without exactly one block is an error) and `site/online.json`.
  `gen_support.py --check` (and so `tools/check_docs.py`) fails when any of the three is stale. `gen_online.py` also runs on its own.
- `docs/reference/support-matrix.md` (generated) links the new page, which keeps it reachable from `docs/index.md` (orphan check).
- `site/`: an "Online play, Napple to Napple" section (`#online`, nav link "Online"), rendered by `app.js` from `online.json` as one card per title
  (modes the game has, with a status chip, note and date; the modes it lacks in one line) plus what each mode means. Without the JSON the static
  link to the docs page stays. No new dependencies. Checked in Chromium at 1280 and 390 px: 4 cards, no horizontal scroll, no console errors.
- `tools/support/test_gen_support.py`: the README and `compat/online.json` are inputs now; four tests for the online outputs (written and
  checked, a data change makes all three stale, an invalid status is rejected, a README without the markers is rejected).

## Seed data (as of 2026-11-02)
- Mario Kart 8 Deluxe: NEX. LAN Play through a room works (two instances); local wireless through a room is broken (station crashes after
  join); online matchmaking through napple-online works (login, online menu, two instances in one gathering); online race partial (pending the
  peer-to-peer fix); friends untested (in progress); rankings and DataStore untested.
- Animal Crossing: New Horizons: NEX; island visits online and local play untested; rankings unsupported (the game has none).
- Super Mario Odyssey: no multiplayer modes (unsupported). Backend recorded as `nex` with rankings and DataStore `untested`, because Luigi's
  Balloon World is an online feature (scores and shared balloons). **Proposed**, from knowledge of the game, not checked on Napple.
- Link's Awakening: backend `none`, every mode unsupported.

## Decisions
- `unsupported` means the game has no such mode; `untested` means it has one nobody has tried on Napple. README and the summary tables show
  `unsupported` as `-` to keep them readable.
- ACNH `lan_play` is `untested` rather than `unsupported`: its local play is wireless and a separate LAN mode is not confirmed.
- Titles are sorted by name in every output, so the order does not depend on the JSON file.
- The landing page renders from JSON at load time (as the task asked) instead of generating HTML into `index.html`.

## Verification
`python3 -I tools/support/gen_support.py`, `python3 -I tools/support/gen_support.py --check` (OK), `python3 tools/check_docs.py` (OK),
`python3 -I tools/support/test_gen_support.py` (9 tests OK). Status: *tested*.
