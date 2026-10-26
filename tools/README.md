# tools/

Build, test and diagnostic scripts. Paths in this file are relative to the `Napple/` repository root; run scripts from there unless noted.
Everything that touches game data expects files you dumped yourself under `work/` (never committed).

## Build and CI

| Script | Purpose | Requirements |
|---|---|---|
| `ci_build.sh` | Builds every in-repo dependency in order, then the CLI, Studio and the release artifacts into `dist/` (what `.github/workflows/release.yml` runs). | macOS on Apple Silicon, Xcode command line tools, rustup/cargo, `brew install opus libvpx` |
| `build_ios.sh` | Builds the iOS / iOS Simulator `napple-runtime` (`SDK=iphonesimulator` for the simulator). | macOS with Xcode, `build_ios_deps.sh` run once |
| `build_ios_deps.sh` | Builds the static libopus (and optionally libvpx) the iOS runtime links, into `build-ios-deps/`. Prints the SHA-256 of what it downloaded. | macOS with Xcode, cmake, network |
| `build_a32jit.sh` | Lifts the AArch32 modules of a game data directory to AArch64 and builds `modules/a32jit.dylib`. | macOS, a prepared data directory |
| `refresh_app_runtime.sh` | Replaces the runtime and launcher inside already built `.app` bundles and re-signs them ad hoc (fast iteration without `napple build`). | macOS, built runtime and launcher |

## Checks

| Script | Purpose | Requirements |
|---|---|---|
| `support/gen_support.py` | Generates `docs/reference/support-matrix.md`, `feature-support.md` and `support-data.json` from `compat/matrix.json`, the title notes and the coverage snapshot (`tools/support/data/`). `--check` fails on stale files (also run by `check_docs.py`); `--refresh-snapshot` imports the newest percent-analysis snapshot. Test: `python3 -I tools/support/test_gen_support.py`. | Python 3 only |
| `check_docs.py` | Verifies every relative Markdown link and `#anchor`, the `docs/symptoms.md` table shape and that every title has a `docs/titles/<id>.md`. Runs in CI. | Python 3 only |
| `linux-check/` | Syntax check and unit tests for the portable part of `runtime/src/services/` on Linux. See `linux-check/README.md`. | gcc or clang on Linux or macOS |
| `boot_test.sh` | Headless boot of a prepared title for N seconds; writes the compat stage file `work/compat/<titleid>/<buildid>/runtime-boot.json`. | macOS, built runtime, a prepared game under `work/games/` |

## Game preparation and data tools

| Script | Purpose | Requirements |
|---|---|---|
| `prepare_game.sh` | Stopgap pipeline: container + `prod.keys` to a runtime data directory (`work/games/<titleid>/<buildid>/`). | your own dump and keys, Python, built rewriter |
| `prepare_tables.py` | Writes the `.tables` and `manifest.json` the loader reads (used by `prepare_game.sh`; see `docs/architecture/module-format.md`). | Python 3 |
| `make_empty_romfs.py` | Writes a valid empty RomFS image for tests that have no game. | Python 3 |
| `gen_a32_nvn.py` | Regenerates `runtime/src/a32/a32_nvn_sigs.inc` from `gfx/src/nvnapi/nvn_protos.h`. | Python 3 |
| `addr2sym.py` | Maps `module+0xoff` to the nearest exported symbol in a data directory. | Python 3, optional `c++filt` |
| `romfs_lookup.py` | Lists the RomFS files a game read in a time window of a `NAPPLE_FS_TRACE` log. | Python 3, a `romfs.bin` |
| `refpy/` | Reference Python NSO rewriter and helpers (`patch_nso.py`, `relr.py`, `nsoimg.py`, `call_window.py`). Own `LICENSE`. | Python 3, `lz4` |

## Performance and flakiness hunts

| Script | Purpose | Requirements |
|---|---|---|
| `long_run_perf.sh` | Headless long run with `NAPPLE_PERF_LOG=1`; reports stalled windows, pipeline builds and RSS. Uses `long_monitor.sh`. | macOS, built runtime, a prepared game |
| `long_monitor.sh` | Samples RSS, threads and CPU of a running `napple-runtime` every N seconds. | macOS or Linux, `ps` |

## dev/

Developer test drivers (not part of the product): GPU-lock wrapper, windowed and packaged-app runs with injected key events, thread dumps via `SIGUSR1`.
See [`dev/README.md`](dev/README.md). `dev/synth_keys.m` is the key injector the drivers use (macOS, needs Accessibility permission for the terminal).

## legacy/

Kept for reference, candidates for removal; nothing in the repository references them.

| Script | Status |
|---|---|
| `flaky_boot.sh` | Repeated headless boots to tell a flaky guest `Break` from a build-specific one. Superseded by `boot_test.sh` for normal use. |
