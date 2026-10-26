# repo-hygiene (2026-10-26 slim batch): root files, layout, legacy removal

Branch `claude/repo-hygiene-2026-10-26-slim`, Linux cloud. No behaviour changes; only repository layout, community files and documentation.

## What changed
- **Legacy removal.** The base commit (`a744b7f`) is tagged `archive/legacy-projects-2026-10`; `LASRecomp/` (548 files) and `MacNativeGames/` (877 files) were then removed in one commit that names the tag. History stays reachable with `git show archive/legacy-projects-2026-10:LASRecomp/...`.
- **New root files** (`Napple/`): `LICENSE` (MIT, Copyright (c) 2026 Eros J. Talevi), `CONTRIBUTING.md`, `SECURITY.md`, `CODE_OF_CONDUCT.md` (Contributor Covenant 2.1), `CHANGELOG.md`, `.editorconfig`, `.clang-format`, `rustfmt.toml`, `.gitattributes`, tidied `.gitignore`.
- **`.github/`**: `pull_request_template.md`, `ISSUE_TEMPLATE/bug_report.yml`, `feature_request.yml` (compat-report.yml and config.yml kept), `dependabot.yml` (cargo, github-actions, swift in `/studio`), `CODEOWNERS` (@erostalevi).
- **README**: badges, mermaid pipeline diagram, per-title status table (every cell copied from `docs/titles/README.md` and `docs/ios.md`), repository layout, FAQ, contribution links, license line.
- **Tools layout**: `tools/dev_*.sh` and `dev_README.md` to `tools/dev/` (with `synth_keys.m`, which only they use; their `cd "$(dirname "$0")/.."` became `../..`), `rescue_icloud.sh` and `flaky_boot.sh` to `tools/legacy/`, new `tools/README.md` (purpose and requirements for every script; `rescue_icloud.sh` flagged obsolete). Tracked `__pycache__/*.pyc` files removed from the index.
- **Process folders** (outer repo): `integrators/done/` (37 files) and `orchestrations/done/` (28 files) moved with `git mv` to `docs/process/archive/{integrators,orchestrations}/`; `integrators/` and `orchestrations/` keep `README.md` and `TEMPLATE.md` and point to the archive. The workflow skills under `.claude/skills/napple-*` and `Napple/docs/tooling.md` were updated to the new paths.

## Decisions (no questions were asked)
- Upstream URL for MacNativeGames is not recorded anywhere in the repo, so the README credit points at the tag `archive/legacy-projects-2026-10` instead of inventing a URL.
- `.clang-format` is LLVM-based with 4-space indent, attached braces, column limit 160 and one-line ifs/blocks, derived from `runtime/src/kernel.c`. Statements packed several per line (common here) are still split by clang-format; that is inherent. `rustfmt.toml` uses edition 2024 and `max_width = 160`. Neither was run over the tree (code-slim owns the format pass).
- CHANGELOG has no invented releases: releases are `v0.1.<run number>` per `release.yml`, and no such tags exist in the repo, so entries are the titles of the notes in `docs/changes/`, grouped by date.
- Dependabot swift directory is `/studio` (where `Package.swift` lives).
- `synth_keys.m` moved with the dev drivers instead of `legacy/`, because they build and call it.
- `docs/process/archive/` is at the OUTER repo root (not under `Napple/docs/`) so it stays out of the public tree and out of docs-consolidate's ownership.

## Grep evidence (done on the base commit, before removal)
- `git grep -nI -e LASRecomp -e MacNativeGames -- . ':!LASRecomp' ':!MacNativeGames'` hits only: prose/credits in `Napple/README.md`, `THIRD_PARTY_NOTICES.md`, `gfx/THIRD_PARTY_NOTICES.md`, `docs/*.md`, `docs/changes/*.md`, source comments (`gfx/src/shader/maxwell_translate.c`, `runtime/src/movie.c/.h`), and `.claude/skills/napple-latest` (a read-only report loop guarded by `[ -d "$r/.git" ]`, so a missing folder is skipped). None in any Makefile, `Package.swift`, `Cargo.toml`, `.github/workflows/*`, or markdown link target.
- Credits and notices are kept (they name the projects, not the folders).
- After the moves: `git grep -n 'tools/dev_\|tools/rescue_icloud\|tools/flaky_boot\|tools/synth_keys'` finds only `tools/dev/README.md` (updated) and old change notes (history, left as written). `git grep -n 'integrators/done\|orchestrations/done'` finds nothing outside the archive and the original assignment file `orchestrations/repo-hygiene.md`.
- `python3 tools/check_docs.py` is green (see integrator file for the final run).

## Verified vs not
- tested: `tools/check_docs.py` (links/anchors); `clang-format --style=file` parses `.clang-format`; rustfmt.toml accepted by rustfmt.
- not verified: the moved dev scripts were not run (macOS-only); workflows were not run, only grepped for paths (they reference `tools/check_docs.py`, `tools/ci_build.sh`, `tools/build_ios*.sh`, all unmoved).

## Needs from other areas
- docs-consolidate: `CONTRIBUTING.md` mentions `docs/guides/`; if the docs layout changes `docs/changes/` or `docs/titles/`, update links in `README.md`, `CHANGELOG.md`, `CONTRIBUTING.md` (check_docs will flag them).
- code-slim: apply `.clang-format` / `rustfmt.toml`.
