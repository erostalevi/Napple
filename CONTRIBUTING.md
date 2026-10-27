# Contributing to Napple

Thanks for helping. Napple is early and moving fast, so small, focused changes with tests are the easiest to review.

## Legal rules first

These apply to every issue, pull request, discussion, screenshot, log and mod. A contribution that breaks them is removed, and the history is cleaned up if it got into a branch.

1. **No keys, firmware, game data or dumps.** Never post or commit `prod.keys`, `title.keys`, firmware, game containers (`.nsp`, `.xci`, `.nsz`, `.nca`, `.nso`, `.nro` of a retail game), RomFS contents, extracted textures, models, audio or shader binaries, amiibo dumps, or memory dumps.
2. **No Nintendo code.** Do not paste decompiled or disassembled game or SDK code, and do not contribute anything written by looking at leaked source or tools. Napple's runtime is written from public documentation, observed behaviour and permissively licensed or original code; reference-only projects are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Code from a GPL project cannot be copied in.
3. **No online services.** Napple has provider hooks and a null (offline) provider only. Do not contribute Nintendo online service code or protocols.
4. **You own what you send.** Only submit work you wrote, or work whose license allows MIT and that you credit (and add to `THIRD_PARTY_NOTICES.md`).
5. **Logs, title ids, versions, build ids and screenshots of your own screen are fine.** Remove personal paths and anything derived from keys first. Small screenshots that show a bug are fine; bulk captures of game content are not.
6. **You need to own the game** to document or test it. Napple does not help you get one.

Not sure whether something is allowed? Ask in a Discussion before posting it. If you think a key, a game file or Nintendo code is already public in this repository, report it privately ([SECURITY.md](SECURITY.md)); do not open a public issue about it.

## Sign-off (DCO)

Napple uses the [Developer Certificate of Origin](https://developercertificate.org/) instead of a CLA. Add a sign-off to every commit:

```sh
git commit -s -m "fix: ..."     # adds: Signed-off-by: Your Name <you@example.com>
```

It says you wrote the change or have the right to submit it under the [MIT license](LICENSE). Use your real name or a stable handle and an address you control. Forgot it? `git commit --amend -s` for the last commit, or `git rebase --signoff` on your own branch. The sign-off is required on every commit; no bot checks it yet, reviewers do.

By contributing you also agree to follow the [Code of Conduct](CODE_OF_CONDUCT.md). Security problems go through [SECURITY.md](SECURITY.md), not public issues.

## Ways to help

| I want to... | Start with | Needs a Mac? | Needs a game? |
|---|---|:-:|:-:|
| Report a bug | the *Bug report* form, [how to capture a log](docs/guides/contributing-report-a-bug.md) | yes | no |
| Report that a game does not boot or looks wrong | *Compatibility report* or *Graphics bug*, [graphics guide](docs/guides/contributing-report-a-graphics-bug.md) | yes | yes |
| Document a title (catalogue page, matrix) | *Document a title*, [guide](docs/guides/contributing-document-a-title.md), [field reference](docs/reference/catalogue-howto.md) | yes | yes |
| Write a mod or a game fix | *Mod or fix submission*, [guide](docs/guides/contributing-write-a-mod.md) | to test | yes |
| Fix a bug or add a service, svc, NVN function or shader op | [guides](docs/guides/README.md), then a pull request | for most | rarely |
| Improve docs, tests or tooling | any Markdown file, `python3 tools/check_docs.py`; Linux is enough | no | no |
| Ask a question or share an idea | Discussions ([how they are used](docs/guides/contributing-community.md)) | no | no |

Look for the `good first issue` and `help wanted` labels ([label set](docs/guides/contributing-community.md#labels)). Say in the issue that you are taking it, so two people do not do the same work.

## Where to start

- Architecture: [`docs/architecture/overview.md`](docs/architecture/overview.md). Runtime services: [`docs/architecture/runtime-services.md`](docs/architecture/runtime-services.md).
- Per-title status and known problems: [`docs/titles/README.md`](docs/titles/README.md). Symptom lookup: [`docs/symptoms.md`](docs/symptoms.md).
- Developer guides (build, test, add a service, add a title) live under [`docs/guides/`](docs/guides/README.md); [`tools/README.md`](tools/README.md) lists the scripts.

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
4. Sign off every commit (`git commit -s`). Fill in the pull request template (what, why, how it was tested, what was not).
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
