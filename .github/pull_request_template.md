## What and why
<!-- One short paragraph. Link the issue: Closes #123 -->

## Kind of change
- [ ] Code fix or feature
- [ ] Catalogue / title page / docs only
- [ ] Mod or game fix (data)
- [ ] Tests only

## How it was tested
<!-- Commands and results. Use the status words: verified (ran it), tested (unit test passes), unbuilt (never compiled on macOS), untested. -->

## What was not tested
<!-- Be explicit: no Mac, no game, no GPU, ... -->

## Checklist
- [ ] Every commit has a `Signed-off-by` line (`git commit -s`, see CONTRIBUTING.md).
- [ ] No keys, firmware, game files, dumps, amiibo data or Nintendo code in the diff (including images and logs).
- [ ] `python3 tools/check_docs.py` passes if I touched Markdown or compat data.
- [ ] Tests added or updated for behaviour changes; none weakened or deleted to get green.
- [ ] Formatted only the files I changed (`.clang-format`, `rustfmt.toml`).
- [ ] Change note added in `docs/archive/changes/` for user-visible or architectural changes.
