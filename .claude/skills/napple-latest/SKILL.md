---
name: napple-latest
description: Survey the Napple work fronts (branches, worktrees, stashes, uncommitted work; read-only, no git changes), then clean-rebuild the decompiler (napple CLI + rewrite/runtime toolchain), Napple Studio and every in-repo dependency (gfx, audio, input, frontend, runtime, launcher) from the latest sources, run the tests, and put the result in ~/Desktop/NappleLATEST. Use when the user asks for "the latest build", "a current build", "sync and build", or to refresh Desktop/NappleLATEST.
---

# Napple LATEST build

Script: `.claude/skills/napple-latest/napple_latest.sh` (run it with an absolute path; it finds the repo itself).

**Repo gotcha:** the git repo is the OUTER folder `/Users/neto/Documents/Napple`. `Napple/.git` is a stale nested
repo (old commit `d19b431`): never run `git` from inside `Napple/` or with `git -C Napple`.

## 1. Survey the work fronts (read-only)

```bash
/Users/neto/Documents/Napple/.claude/skills/napple-latest/napple_latest.sh status
```
It lists (without fetching): main vs the last-known origin/main, worktrees, every local/remote branch with commits not in `main`
(cloud sessions push `origin/claude/*`), stashes, uncommitted files in each checkout, and the sibling repos
(LASRecomp, MacNativeGames — not part of this build; just report them).

Also check for Claude sessions still running on this project (`mcp__ccd_session_mgmt__list_sessions`); a running
session may be mid-edit, and the build (which `make clean`s the shared checkout) can break its runs. Mention it and ask before building.

## 2. Local only: no git changes

The build is the main checkout's working tree exactly as it is on disk, uncommitted edits included
(they are recorded in `BUILD_INFO.txt`). This skill never runs `git fetch`, `pull`, `merge`, `rebase`,
`stash`, `checkout`/`switch`, `branch`, `commit` or `push`, and creates no branches or worktrees, even if the
survey shows work elsewhere. Work that lives only in another branch, worktree or stash is NOT in the build:
list it in the report so the user can bring it in themselves.

## 3. Build, test, package

```bash
/Users/neto/Documents/Napple/.claude/skills/napple-latest/napple_latest.sh build
```
Run it in the background (a clean build + tests takes several minutes) and wait for the notification.
Options: `--incremental` (skip the clean), `--no-tests`, `--dest DIR`.

What it does: `make clean` + rebuild of gfx (lib + runtime adapter), audio, input, frontend, runtime, launcher;
`cargo clean --release` + release build of the whole Rust workspace (`napple` CLI); a fresh Swift build of
NappleStudio.app (with the CLI, runtime, launcher and compat matrix inside); then `cargo test`, each
`make test`, and `swift test` (a failing test is recorded, not fatal; a failing build step stops everything).
Builds happen in `$TMPDIR` because File Provider xattrs on ~/Documents and the iCloud Desktop break codesign.

Output `~/Desktop/NappleLATEST/`: `NappleStudio.app`, `bin/` (napple, napple-runtime, napple-launcher),
`lib/` + `include/` (the static libraries and headers), `compat/`, `knownSignatures/`, `logs/`, `BUILD_INFO.txt`
(commit, included uncommitted files, toolchains, test results, sha256s). A previous NappleLATEST is moved to
the Trash, not deleted. Opus and libvpx are Homebrew static libraries linked in; they are not rebuilt.

## 4. Report

Tell the user: work that exists only outside the working tree (other branches, worktrees, stashes) and so is not in the build, commit + number of uncommitted
files built, every test FAIL with the first useful lines of its log, and whether the app signature verified
at the destination. Do not call the build "tested in a game": these are unit/integration tests only.
