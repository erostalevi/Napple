---
name: ura-napple-parallel
description: Master guide for a Napple parallel session (local Mac or Linux cloud). Tells the session how to start up, how to behave given its environment's limits, how to stay inside its own area, and how to finish - commit, write its integrator file in ./integrators, and push its branch - so the integrator session (napple-integrate) can land the work. Use at the start of every parallel Napple session, when the prompt says "parallel", "branch session" or names this skill, or when the user asks how a parallel session should behave.
---

# Napple parallel session: startup, behaviour, hand-off

You are one of several sessions working at the same time on separate branches. A later integrator session (`/napple-integrate`) will merge everyone's work
into `main` on the user's Mac. Your job: do your area well, never step on anyone else, and leave a hand-off file that lets the integrator verify and land your work
without having to ask you anything.

**Repo gotcha:** the git repo is the OUTER folder (on the Mac `/Users/neto/Documents/Napple`). `Napple/.git` is a stale nested repo: never run `git` inside `Napple/`.
In a cloud checkout the repo root may be the `Napple/` folder itself: then paths have no `Napple/` prefix; use whichever layout you actually see.

## 1. Startup (do this first, before editing anything)
1. Know your assignment. Normally your prompt says "your assignment is `orchestrations/<name>.md`" (written by `/napple-orchestrate`): read that file fully first. It gives your branch, area,
   owned paths, do-not-touch paths, starting points, acceptance, `depends_on`, `base`, `orchestrator_session`, env and model. Treat it as the brief; the prompt may be just a pointer to it.
   **Never edit `orchestrations/`** (the orchestrator and integrator own those files; editing them makes merges conflict). If no orchestration file exists, the prompt must name a branch, an area,
   the files you own and a goal. If any of those is missing, stop and ask the user. Do not guess an area.
   If the file's `env` differs from the environment you detect in step 2 (for example it says `mac` and you are on Linux), say so in your first message and only do the part that is honest on this env.
2. Detect the environment and say it in your first message:
   ```bash
   uname -sm; git rev-parse --show-toplevel; git branch --show-current; git log --oneline -1; git status --short | head
   ```
   - `Darwin` = **Mac (local)**. Metal, CoreAudio, VideoToolbox, Mach VM, codesign and real games are available.
   - `Linux` = **cloud**. No Metal, no CoreAudio, no macOS headers, no Mach calls, no display, no games or firmware.
2b. **Remote Control (local Mac sessions only):** so the user can follow and steer you from their phone, turn Remote Control on for yourself at startup. Load the tool with ToolSearch (`select:mcp__ccd_session_mgmt__set_remote_control`) and call it with `session_id: "self"` and `enabled: true`. If the tool is missing, the call is refused, or the state comes back `unavailable`, continue without it and say so in your first message; never retry in a loop. Cloud sessions do not need this (they already show in the user's cloud session list).
3. Make sure you are on your own branch (the file's `branch`), based on the file's `base` or the commit the prompt names (or current `main`). Never work on `main`. Never touch another session's branch or worktree.
4. Read before writing: `docs/architecture/overview.md`, the doc for your area (`docs/architecture/runtime-services.md`, `docs/architecture/gfx.md`, ...) and the newest notes in `docs/archive/changes/`.
5. Check what builds here, once, and record it: try your area's build and tests (`make -C <area>`, `cargo build/test`, ...). Write down exactly what builds and what does not in this
   environment; this becomes the "Built and tested" section of your hand-off file.

## 2. Ownership and collisions
- Edit only the paths you own. Shared files (`docs/architecture/runtime-services.md`, `COMPATIBILITY.md`, `Cargo.toml`, top-level Makefiles (Cargo.lock is not tracked)): append-only and minimal. Never reformat, rename or reorder.
- Need a change in someone else's area? Do not make it. Write it in your hand-off file under "Needs from other areas".
- New files are fine inside your area. Put your change note in `docs/archive/changes/<date>-<branch-without-claude/>.md`, not in an existing note. Public files (notes, docs, code comments) must not name Claude, sessions, `integrators/` or `orchestrations/`: say "branch <name>" and describe results directly (publish.sh refuses a tree that does).
- Do not edit `integrators/` files that are not yours, and never edit anything in `orchestrations/`. Do not run `napple-latest`, `napple-integrate` or `percent-analysis`; those belong to other sessions.

## 3. Behaviour by environment

**Mac (local)**
- Build and run for real. Prefer headless scripted runs; log what you ran (command, game, result). Use the debug env vars (`NAPPLE_INPUT_LOG=1`, `NAPPLE_GFX_CMDTRACE=1`, `NAPPLE_DRAWLOG_RT_ALL=1`, ...) to prove your feature was hit.
- Builds go through the existing scripts/targets, in `$TMPDIR` where they say so (File Provider xattrs on ~/Documents break codesign). Do not clean-build the shared checkout while other sessions may be running; use your own worktree.
- Do not leave a long-running game process open when you finish.

**Linux cloud**
- You cannot build or run macOS-only code (`.m` files, Metal, CoreAudio, Mach, VideoToolbox, AppKit/Swift Studio, the native runtime). Do not pretend to. Do not stub macOS headers into the repo to make it compile.
- Do: pure logic in portable C or Rust with unit tests on Linux (parsers, tables, state machines, DSP maths, shader translator, IPC/svc handlers' logic, error-code tables). Keep the IPC/svc/glue code thin.
- For code that cannot build here: `clang -fsyntax-only` / `gcc -fsyntax-only` with stub headers kept OUTSIDE the product tree (for example `tools/linux-check/`) catches typos; say that is all it proves.
- Write portable C for the logic; write macOS calls only where the area needs them, in the style of the neighbouring files, and mark them. The integrator will port Linux-only calls (`epoll`, `/proc`, `clock_gettime` pacing, `getrandom`, ALSA/pulse) to macOS-native ones, so avoid them where an obvious portable form exists, and list any you did use in the hand-off file.
- One commit per logical piece (per service, per op, per file group). Start the subject with `unbuilt:` when the commit contains code that was never compiled on macOS. This makes the integrator's bisect cheap.

**Both**
- Honest status words only: *verified* (ran it here), *tested* (unit test passes here), *unbuilt* (never compiled on macOS), *untested* (compiled, not run), *proposed* (reasoned, not compiled). A feature that is not verified is partial for coverage purposes (`/percent-analysis`).
- Do not weaken or delete tests to get green. Do not hide a failing test: put it in the hand-off file.
- Do not push to `main`, merge into `main`, rewrite history, or force-push. Do not create PRs unless asked.
- If you find a bug outside your area, note it in the hand-off file; do not fix it.

## 4. Finishing (all of this, in this order)
1. Everything committed on your branch; `git status --short` clean (except files you intentionally leave out; say which).
2. Your change note in `docs/archive/changes/<date>-<branch-without-claude/>.md` (no Claude/integrator/orchestration wording, see section 2): what changed, what is verified vs not, files.
3. Your **integrator file**: copy `integrators/TEMPLATE.md` (at the git root; it is the source of truth for the format, the copy below is a fallback if the folder is missing) to `integrators/<branch-with-slashes-as-dashes>.md` and fill every section. Set `orchestration: <name>` in the front matter when you were given an orchestration file (else `none`), and check each item of its "Acceptance" list in "Built and tested here" with an honest status. It must be self-contained: the integrator has no memory of your session.
4. Commit it (`docs: integrator file for <branch>`) and **push your branch**:
   ```bash
   git push -u origin <your-branch>
   ```
   Cloud sessions MUST push; their work is lost otherwise. Mac sessions push too. Pushing your own branch is authorized by this skill; pushing anything else is not.
5. Tell the orchestrator, if the file names one (`orchestrator_session` not `none`): `SendMessage`/`mcp__claude-code-remote__send_message` to that session with `<name> handed off: <branch> <head>` (best effort; if the tool is unavailable or fails, skip it, the pushed branch and integrator file are the real hand-off).
6. Final message: branch name, head commit, the path of the integrator file, and a five-line summary (done / not done / unbuilt / needs from others / risks).

### Integrator file template (`integrators/<branch>.md`)
```markdown
---
branch: <full branch name>
orchestration: <name from orchestrations/<name>.md, or none>
base: <commit hash the branch started from>
head: <head commit hash at hand-off>
env: mac | linux-cloud
date: YYYY-MM-DD
areas: [gfx, runtime/services, ...]
status: ready | ready-with-caveats | blocked
---

# <branch>: <one-line goal>

## Summary
What was done, in plain words (5-10 lines). What was NOT done from the assignment.

## Files
Owned/changed paths (from `git diff --stat <base>..HEAD`), new files, deleted files. Shared files touched (append-only).

## Built and tested here
| Item | Status (verified / tested / unbuilt / untested / proposed) | Evidence (command + result) |

## Unbuilt / untested list (the integrator checks these first)
Every file, service, op or function that was never compiled or run on macOS, one per line, with its commit hash.

## Findings
Bugs found, surprises, decisions you made and why, things the docs got wrong, bugs outside your area (not fixed).

## How to integrate
- Merge order constraints (depends on / must land before or after <branch or area>).
- Conflict hotspots (files other branches probably touch) and how to resolve them.
- Build commands for this work, in order, with the expected result.
- Linux-only calls you used that should be ported to macOS-native on the Mac (file:line + suggested native call).
- Code that should be left as portable C on purpose (and why).

## How to test
Numbered checks, each with the command, what passing looks like, what failing looks like:
1. Unit/regression tests.
2. Headless scripted runs (games, flags, env vars) and the log lines proving the feature was hit.
3. Regression guard: what else must still pass because of what you touched.
4. Anything that needs a human (what to look at, which scene).

## Fix policy hints
Which failures are safe for the integrator to fix directly (typos, includes, signatures) and which need a design decision (and what the options are).

## Needs from other areas
Changes you needed but did not make because they are not yours.
```
