# Running the tests

## Goal
Know which command runs which suite, what each proves, and what can only run on a Mac. Target names below were checked against the Makefiles, `Cargo.toml`, `studio/Package.swift`, `tools/ci_build.sh`
and `.github/workflows/tests.yml`. Results marked "measured" were produced on a Linux box (no Metal, no Xcode) while writing this page.

CI (`.github/workflows/tests.yml`, on every pull request and push to `main`, plus manual dispatch) has these jobs:

| Job | Runner | Command(s) | Blocking |
|---|---|---|---|
| `docs` | ubuntu-latest | `python3 tools/check_docs.py` | yes |
| `rust` | macos-26 | `cargo test --workspace --locked` | yes |
| `native` | macos-26 | `make -C input test`, `make -C audio test`, `make -C frontend test`, `make -C gfx lib adapter`, `make -C gfx tests`, `make -C gfx test`, `make -C runtime test test-services` | yes, except `make -C gfx test` (`continue-on-error`: a virtual-Mac runner may lack a usable GPU; it stays a local gate) |
| `studio` | macos-26 | `swift test --package-path studio` | yes |
| `ios-smoke` | macos-26 | `SDK=iphonesimulator tools/build_ios_deps.sh`, `SDK=iphonesimulator tools/build_ios.sh` (compile and link only) | no (`continue-on-error` until the `a32` sources are compiled by `tools/build_ios.sh`) |

Not in CI: anything that needs game files, keys or firmware (boots of the reference titles), `swift test` UI automation, signing, `make -C runtime test-host`, `cargo clippy`.

## Quick matrix: Linux or macOS
| Suite | Command | Linux | macOS |
|---|---|---|---|
| Docs | `python3 tools/check_docs.py` | yes (CI runs it on Linux) | yes |
| Rust workspace | `cargo test --workspace --locked` | yes (measured: every test binary reports ok) | yes (CI) |
| Rust lints | `cargo clippy --workspace` | runs; measured: fails with 4 `clippy::never_loop` errors in `napple-build` (`firmware.rs`, `legacy.rs`, `pipeline.rs` x2). Not part of CI | same code, same lints |
| Runtime unit tests | `make -C runtime test` | no | yes (CI) |
| Runtime service tests | `make -C runtime test-services` | no (see `tools/linux-check` for the portable subset) | yes (CI) |
| Runtime host tests | `make -C runtime test-host` | measured: does not build (`src/threads.c` uses Mach APIs); only `test_ios_dylib` builds | yes (run by `tools/ci_build.sh`, not by `tests.yml`) |
| Service logic on Linux | `sh tools/linux-check/run-tests.sh`, `sh tools/linux-check/check-syntax.sh` | yes | not needed |
| gfx (Metal) | `make -C gfx test` | no | yes (local gate; CI non-blocking) |
| gfx without Metal | `sh gfx/tests/t_getters_linux.sh`, `sh gfx/tests/t_xlate_linux.sh` | yes | the first delegates to the Makefile; the second also runs there |
| Audio | `make -C audio test` | no (CoreAudio, AudioToolbox, Homebrew `opus`) | yes (CI) |
| Input | `make -C input test` | no through the Makefile (it links GameController and uses `clock_gettime_nsec_np`); `tests/test_hid.c` compiles alone with `src/hid_shm.c` | yes (CI) |
| Frontend | `make -C frontend test` | no (Cocoa) | yes (CI) |
| Studio | `swift test --package-path studio` | no (platform macOS 26) | yes (CI) |
| Full build + tests | `tools/ci_build.sh [dir]` | no | yes |

## Rust (`cargo test`, clippy)
- Workspace members (`Cargo.toml`): `napple-core`, `napple-cli`, `napple-rewrite`, `napple-compat`, `napple-sigs`, `napple-build`. Unit tests sit next to the code (`#[cfg(test)]`); integration tests in
  `crates/napple-build/tests/` (`e2e.rs`, `cancel.rs`, `golden/`), `crates/napple-core/tests/ingest.rs`, `crates/napple-rewrite/tests/`. None needs a real game: fixtures are synthetic.
- `cargo test --workspace --locked` is the CI command (`--locked`: `Cargo.lock` must not change). One package: `cargo test -p napple-sigs` (40 tests, offline). On a shared machine set `CARGO_TARGET_DIR` to keep `target/` out of the tree.
- `cargo clippy --workspace`: see the matrix; fix new warnings you introduce, and do not treat the four existing `never_loop` errors as yours.

## Runtime (macOS)
`make -C runtime` builds `runtime/build/napple-runtime`; it needs the other libraries built first: `make -C input && make -C audio && make -C frontend && make -C gfx lib adapter` (order of `tools/ci_build.sh`;
`GFX=napple` is the default, `GFX=stub` selects `gfx_stub.c`). `debug=1` builds with `-O0`.
- `make -C runtime test` builds and runs `build/tests/test_media`, `test_ipc` (CMIF, domains, TIPC, `sm`), `test_fs_journal`, `test_hid_service`, `test_timescale`; each prints a JSON line `{"test":...,"checked":N,"failed":M}` and the target stops at the first failure.
- `make -C runtime test-services` builds every `runtime/tests/test_svc_*.c` (32 files today) plus `test_hid_depth` (macOS only: `hid.c` needs Mach and the input library) and runs them. A test `#include`s the service source it exercises; the object of that
  source is left out of the link (`runtime/tests/services.mk`). How to write one: [add-ipc-service.md](add-ipc-service.md).
- `make -C runtime test-host` builds the portable kernel, loader and service code against the stand-ins in `runtime/tests/host/` and runs `test_svc_async`, `test_kernel_ipc`, `test_loader`, `test_nvdrv`, `test_fs_savedata`, `test_fs_journal`,
  `test_ios_dylib`, `test_kernel_sched`. The Makefile describes it as "any POSIX host, e.g. Linux", but `src/threads.c` (in `HOST_KERNEL`) contains Mach calls in the `NAPPLE_PCPROF` sampler, so on the Linux box used for this page only `test_ios_dylib`
  builds (`make BUILD=<dir> <dir>/host/test_ios_dylib`). Treat it as a macOS target. `HOST_CC` picks the compiler. How to write one: [add-svc.md](add-svc.md).
- Manual tools, not wired to any target: `runtime/tests/a32/` (`test_module.c` AArch32 loader against a real data dir, `a32_boot.c`, `fuzz.py` interpreter fuzzing against Unicorn) and `runtime/tests/a32jit/` (`difftest.c` lifter versus interpreter,
  `ittest.c`, `bench.c`; build lines are in their headers; `tools/build_a32jit.sh` builds the lifted jit).

### Linux subset: `tools/linux-check`
`runtime/` only builds on macOS; `tools/linux-check` checks the portable service code on Linux (details in `tools/linux-check/README.md`):
- `sh tools/linux-check/run-tests.sh [runtime/tests/test_svc_x.c ...]` compiles each `test_svc_*.c` with every `src/services/*_core.c` and `ipc_mock.c` (a stand-in for `ipc.c`, `kernel.c`, `log.c`) and runs it. Measured: 29 of 32 pass with `"failed":0` (33 of 36 after sim-online added `test_svc_online`, `test_svc_irs_sim`, `test_svc_amiibo`, `test_svc_nfp`);
  `test_svc_fs` and `test_svc_fs_dataid` do not link (`romfs_map.c` is not part of the script), `test_svc_time` does not link (`host_symbols_register`). Sanitizers:
  `ASAN_OPTIONS=detect_leaks=0 CC="gcc -fsanitize=address,undefined" sh tools/linux-check/run-tests.sh`.
- `sh tools/linux-check/check-syntax.sh [file.c ...]` runs `-fsyntax-only` with gcc and clang over `src/services/*.c` with stub headers (measured: "syntax check: all clean", with warnings in `hid.c`, `ncm.c`; skips `fs.c`, `nvdrv.c`).
Passing says the C is well formed and the core logic matches the test; it says nothing about the macOS build or a game.

## Graphics (`gfx/`)
- `make -C gfx lib adapter` builds `gfx/libnapple_gfx.a` and `gfx/build/obj/adapter/gfx_napple.o` (what the runtime links). `make -C gfx tests` builds every `tests/t_*.c` and `t_*.m` to `gfx/build/tests/`. `make -C gfx test` runs them
  with `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=0`, then `t_triangle` twice with a disk cache (`T_CACHE_PASS=1|2`) and `t_pipe_stress` (pass 1, then pass 2 with 1 and 4 threads and `NAPPLE_GFX_NO_ARCHIVE=1`), and ends with `ALL TESTS PASSED`.
  They are headless Metal probes: they need a Mac with a GPU and need no game data. One test: `make -C gfx build/tests/t_state && gfx/build/tests/t_state`.
- Linux: `sh gfx/tests/t_getters_linux.sh` (measured `33 checks, 0 failed`: set/get round trips of NVN state and sampler getters, from `api_state.c` and `api_texture.c`) and `sh gfx/tests/t_xlate_linux.sh` (measured `t_xlate: 50 Metal compile checks skipped`, `t_xlate: ok`:
  the Maxwell translator's text assertions, no Metal compile). Writing them: [add-nvn-function.md](add-nvn-function.md), [add-maxwell-shader-op.md](add-maxwell-shader-op.md).
- Corpus runs of real shaders: `NAPPLE_SHADER_CORPUS=<dir>` with `t_xlate`; the corpus lives outside the repository.

## Audio, input, frontend (macOS)
- `make -C audio test` runs `test_audren test_effects test_reverb3d test_review test_audin test_audout test_hwopus test_engine test_threads test_speed` (needs Homebrew `opus`, `OPUS=` overrides the prefix). `make -C audio tsan` and `make -C audio asan`
  rebuild subsets with ThreadSanitizer / AddressSanitizer (not in CI). `make -C audio demo` plays a sine wave.
- `make -C input test` runs `test_hid test_bindings test_motion test_input` (`-Werror` build). The pure HID writer test also compiles on Linux by hand:
  `gcc -std=gnu11 -Iinput/include -Iinput/src input/tests/test_hid.c input/src/hid_shm.c -lm -o /tmp/t && /tmp/t` (measured: 199 checks, 0 failed). `make -C input demo` is an interactive pad demo.
- `make -C frontend test` runs `test_settings` and `test_window` (creates the frontend without showing the window and runs the real AppKit loop). `tests/test_ios_logic.c` is not part of the Makefile `test` target.

## Studio (macOS 26)
`swift test --package-path studio` runs `NappleStudioTests` (`studio/Tests/NappleStudioTests/StudioTests.swift`, `LibraryTests.swift`) against `NappleStudioCore`, the testable logic of the SwiftUI app (`studio/Package.swift`, platform `.macOS("26.0")`).
UI automation is not run in CI. `studio/build_app.sh` builds the app (part of `tools/ci_build.sh`).

## Docs
`python3 tools/check_docs.py [--root <Napple dir>]` (no dependencies, no network, exit 0 = ok) checks that every relative link and `#anchor` in the Markdown files resolves, every document under `docs/` is reachable from `docs/index.md` (no orphans;
`docs/archive/changes/*` is history and only needs a line in its README), the required files exist (`docs/index.md`, `docs/architecture/overview.md`, `docs/guides/README.md`, `docs/reference/cli.md`, `docs/reference/environment-variables.md`,
`docs/reference/glossary.md`, `docs/process/README.md`, `docs/archive/changes/README.md`, `docs/symptoms.md`), code fences are closed, `docs/symptoms.md` rows have four non-empty cells, and every reference title has `docs/titles/<id>.md`.
Run it after every docs change; the `docs` job fails the pull request otherwise.

## Whole build: `tools/ci_build.sh`
`tools/ci_build.sh [output-dir]` (macOS, Apple Silicon): `cargo build --release -p napple-cli --locked`, `make` in `input`, `audio`, `frontend`, `gfx lib adapter`, `runtime`, `packaging`, then (unless `NAPPLE_SKIP_TESTS=1`; failures only warn)
`cargo test --release --locked && make -C runtime test-host`, then `studio/build_app.sh` and `studio/packaging/make_release.sh` into `dist/`. Env: `NAPPLE_VERSION`, `NAPPLE_BUILD`, `NAPPLE_SIGN_IDENTITY`.
Needs Xcode command line tools, rustup, `brew install opus libvpx`. `.claude/skills/napple-latest/napple_latest.sh` runs the same commands plus a status survey.

## iOS
`SDK=iphonesimulator tools/build_ios_deps.sh && SDK=iphonesimulator tools/build_ios.sh` builds `runtime/build-ios-sim/napple-runtime` for the Simulator (compile and link; nothing is run). See [ios.md](../architecture/ios.md).

## Titles
Booting the reference titles needs the user's own game files and keys, so it is never automated: use [debugging.md](debugging.md), section 8, and the pages in [titles/](../titles/README.md). A passing boot is exit 0, thousands of frames and no `CRASH`.

## End-to-end (`tools/e2e/run.sh`)

`tools/e2e/run.sh <las|smo|acnh|mk8d|all>` ties the whole product together on a Mac that has the Testing Material, a Studio keyset (`~/Library/Application Support/Napple/keysets/22.1`)
and this checkout's CLI, runtime, launcher and applet library built (`cargo build --release -p napple-cli`, `make -C runtime applet-lib`, `make -C packaging`). Per title it
builds the .app **through Studio's own build path** (`napple-studio-build`: `KeysetManager` + `NappleCLI.build`, in-place data mode, updates and DLC as planned), then runs the
steps below and prints a PASS / FAIL / KNOWN / REPORT / N/A table plus `summary.json` and `summary.md` (default folder `$TMPDIR/napple-e2e`, `--out` to change it):

| Step | Passes when |
|---|---|
| build | Studio's pipeline produced the .app |
| bundle | `codesign --verify` passes, `Contents/Frameworks/libnapple-applet.dylib` is there, the A32 jit dylib is embedded for the 32-bit title (MK8D) |
| camera-mic | `NSCameraUsageDescription` and `NSMicrophoneUsageDescription` in Info.plist, the camera and audio-input entitlements, no App Sandbox |
| mii-db | (first title) the real Mii Maker applet, driven by `NAPPLE_INPUT_SCRIPT` and `NAPPLE_SWKBD_ANSWER`, adds a Mii to the shared `MiiDatabase.dat`; every later boot reads the same file |
| boot | the launcher starts the app headless, exit 0, no `CRASH`, at least a frame floor |
| audio | the PCM counter line (`[napple-audio] PCM: ...`) saw samples above the noise floor |
| input | the scripted presses reached the guest (`input: script: press`) |
| amiibo | `NAPPLE_AMIIBO=<model id>,3000,12000`: placed and removed, a virtual amiibo in the store (N/A for a title that never opens `nfp:user`) |
| save | a save was committed, and a second boot with the same save root mounts it again |
| overlay | `NAPPLE_FE_OVERLAY` + `NAPPLE_FE_VIEWSHOT` render the overlay tabs to PNGs (a windowed run: the window appears briefly) |
| golden | `tools/golden` routes against the approved goldens: **report only**, golden diffs are not deterministic run to run and never fail the table |

All steps share one system data root (`<out>/support`, via `NAPPLE_SUPPORT_DIR`, see [system data folders](../reference/system-data-folders.md)). Known issues are tagged KNOWN, not
FAIL, with the reason: ACNH's intermittent guest Break at `sdk+0xc80730` right after a headless start (retried once). A `.app` is about 6 GB: apps are deleted after each title
unless `--keep-apps`. Nothing from the Testing Material is copied into the repository.

## Which tests to run for a change
| You changed | Run |
|---|---|
| `crates/*` | `cargo test --workspace --locked` |
| `knownSignatures/` | `napple sigs validate <file>`, `cargo test -p napple-sigs` ([add-signatures.md](add-signatures.md)) |
| `runtime/src/services/*` | `make -C runtime test-services` (Mac) or `sh tools/linux-check/run-tests.sh` (Linux) |
| `runtime/src/kernel.c`, `threads.c`, `ipc.c`, `memory.c`, `ksession.c` | `make -C runtime test test-host` (Mac) |
| `gfx/src/api_*.c`, `mtl_*.m` | `make -C gfx test` (Mac); `sh gfx/tests/t_getters_linux.sh` for state getters |
| `gfx/src/shader/*` | `sh gfx/tests/t_xlate_linux.sh`, then `make -C gfx test` |
| `audio/`, `input/`, `frontend/` | the matching `make -C <dir> test` |
| `studio/` | `swift test --package-path studio` (use `--scratch-path "$TMPDIR/..."` on ~/Documents: File Provider xattrs break its codesign step) |
| anything that crosses Studio, the runtime and packaging | `tools/e2e/run.sh <title>` (end-to-end, needs the Testing Material) |
| any `*.md` | `python3 tools/check_docs.py` |
| `compat/matrix.json`, `docs/titles/*`, the coverage snapshot | `python3 -I tools/support/gen_support.py` (regenerates the support tables), then `python3 tools/check_docs.py` |
