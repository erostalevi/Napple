# docs-consolidate (2026-10-26, slim batch): one navigable documentation set

Branch `claude/docs-consolidate-2026-10-26-slim`, Linux cloud. Docs only; no source code touched. Statuses: *tested* = a check run here passed.

## What changed
- New layout under `docs/`: `index.md`, `architecture/`, `guides/`, `reference/`, `process/`, `archive/changes/`; `symptoms.md` and `titles/` stay in place. Everything moved with `git mv`.
  Old to new: ARCHITECTURE to architecture/overview, runtime-modules to architecture/module-format, rewrite-format, compat-format, runtime-services, studio-protocol, ios to architecture/,
  gfx-design + gfx-interface merged into architecture/gfx, audio-input-frontend-interface to architecture/audio-input-frontend, reference to reference/subsystems, tooling to reference/tooling, changes/ to archive/changes/.
- New pages: overview rewritten against the code (the old one was a "draft v0, nothing implemented" plan) with mermaid diagrams; architecture/aarch32 (from three notes, checked against `runtime/src/a32*`); 8 contributor guides + index;
  reference/cli, environment-variables (about 190 variables from grep of getenv), glossary; process/README; archive/changes/README (index of the 60 notes by theme).
- Stale statements removed or fixed (compared with code): gfx status/TBD lines, module-format "no runtime support for the delta layer" (fs.c has `open_romfs_delta`), runtime-services "H.264 not implemented", "UNBUILT"/"Untested on macOS" banners that later Mac rounds superseded,
  DecoderType value for AVC (1, not 0), ios.md "unbuilt" rows, `NAPPLE_TRACE_IPC` accepting a service name (only `all`), `NAPPLE_INPUT_SCRIPT` times (seconds, not frames).
- `tools/check_docs.py`: also checks required files, closed code fences and that every document under docs/ is reachable from docs/index.md (orphans). Archived notes are history: their links are not checked, each must be listed in their README.
- Site: `docs-site/` (mkdocs-material 9.5.50 / mkdocs 1.6.1 pinned, `hooks.py`), `.github/workflows/pages.yml` builds the docs and publishes them under `/docs/` next to the landing page (docs check runs first), `site/index.html` links to `docs/`. Mermaid is rendered by material in the browser.
- Decisions: the change notes themselves are unchanged; new notes go in `docs/archive/changes/` and get an index line. A hook turns links that leave `docs/` into GitHub links and shows dead links inside archived notes as plain text.
  `docs/process/archive/` is described but not created (repo-hygiene moves the old integrator and orchestration files there).

## Verified
- tested: `python3 tools/check_docs.py` has only the two `README.md:139` failures (links to the old paths; README is repo-hygiene's). All pages, anchors, orphans pass.
- tested: `mkdocs build --strict -f docs-site/mkdocs.yml` in a fresh venv: zero warnings (internal links and anchors validated), 7 MB output, mermaid blocks present.
- tested: a path script confirmed backticked repo paths in the new pages exist. Not run: any code, macOS targets.
- untested: the pages.yml workflow itself (needs GitHub Actions).
