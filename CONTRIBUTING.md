# Contributing to Napple

Thanks for helping. Napple is early and moving fast, so small, focused changes with tests are the easiest to review.

By contributing you agree that your work is released under the [MIT license](LICENSE) and that you follow the [Code of Conduct](CODE_OF_CONDUCT.md).
Security problems go through [SECURITY.md](SECURITY.md), not public issues.

**Never attach or commit game files, `prod.keys`, firmware or anything derived from them.** Logs, titles ids, versions and screenshots of your own
screen are fine.

## Where to start

- Architecture: [`docs/architecture/overview.md`](docs/architecture/overview.md). Runtime services: [`docs/architecture/runtime-services.md`](docs/architecture/runtime-services.md).
- Per-title status and known problems: [`docs/titles/README.md`](docs/titles/README.md). Symptom lookup: [`docs/symptoms.md`](docs/symptoms.md).
- Compatibility reports are welcome as an issue (the *Compatibility report* form): stage reached, evidence, missing services or NVN calls.
- Developer guides (build, test, add a service, add a title) live under `docs/guides/` once the documentation consolidation lands; until then the
  reference docs above and [`tools/README.md`](tools/README.md) are the source of truth.

## Build

Requires an Apple Silicon Mac, the Xcode command line tools, a Rust toolchain (2024 edition) and, for the release build, `brew install opus libvpx`.
The portable parts (Rust crates, the host tests of the runtime, the docs check) also build on Linux.

Build order for the native components: audio, input, frontend, gfx, then `make -C gfx adapter`, then runtime.

```sh
cargo build --release -p napple-cli
make -C audio && make -C input && make -C frontend && make -C gfx && make -C gfx adapter
make -C runtime
make -C packaging
studio/build_app.sh        # optional: Napple Studio
```

`tools/ci_build.sh` runs the whole sequence the way the release workflow does.

## Test

Run what applies to the area you touched before opening a pull request:

```sh
cargo test --workspace
cargo clippy --workspace --all-targets
make -C runtime test test-services test-host   # test-host and test-services are portable to any POSIX host
make -C gfx test                               # macOS only
(cd studio && swift test)                      # macOS only
python3 tools/check_docs.py                    # every relative link and anchor in the Markdown files
```

Linux contributors can use `tools/linux-check/` (syntax check and unit tests for the portable service logic). CI (`.github/workflows/tests.yml`) runs the
same commands. If a test cannot run on your machine, say so in the pull request instead of leaving it out silently.

## Pull request flow

1. Fork, then branch from `main` (`feature/<topic>` or `fix/<topic>`).
2. Keep the change focused: one logical change per pull request, tests with it, docs updated if behaviour or a format changes.
3. Add a change note in `docs/archive/changes/<date>-<topic>.md` for anything user visible or architectural: what changed, what is verified and what is not.
4. Fill in the pull request template (what, why, how it was tested, what was not).
5. Be honest about status: *verified* (ran it), *tested* (unit test passes), *unbuilt* (never compiled on macOS), *untested*, *proposed*.
   Do not weaken or delete tests to get green.

Napple stays game-agnostic: per-game knowledge belongs in data (signatures, patch tables), not in per-game code branches.

## Code style

- Formatting is defined by [`.clang-format`](.clang-format) (C, headers, Objective-C), [`rustfmt.toml`](rustfmt.toml) (Rust) and
  [`.editorconfig`](.editorconfig). Format only the files you change.
- Match the surrounding code: naming, comment density and idiom. Comments say why, not what.
- C: portable C99/C11 for logic, with macOS-only calls (Mach, Metal, CoreAudio) isolated in the files that need them.
- Rust: `cargo clippy` clean, no `unwrap` on input from outside the process.

## Commit messages

- Short imperative subject (about 70 characters), a blank line, then a body saying why.
- Prefix `unbuilt:` when the commit contains code that was never compiled on macOS.
- Reference the issue or the change note when there is one.
