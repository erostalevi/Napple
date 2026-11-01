---
name: napple-integrate
description: Integrate finished parallel Napple work (branches and worktrees from local and cloud sessions started with ura-napple-parallel, whatever area or feature they cover) into local main, guided by each branch's integrators/*.md hand-off file (moved to docs/process/archive/integrators/ once landed) - build and test each branch on this Mac, fix only small macOS-specific problems, port Linux-only C to macOS-native APIs where that makes sense, merge one branch at a time with re-tests, then run napple-latest for the final build. Reports instead of fixing when a branch is heavily broken. It also reads `orchestrations/<name>.md` (written by napple-orchestrate) and moves landed ones to `docs/process/archive/orchestrations/`, and can be scoped to one batch or some names. Use when the user says the parallel sessions are done, or asks to "integrate", "merge the branches", "bring everything to main", "test the cloud work on the Mac", or "land the parallel work".
---

# Napple integrate: branches to main, verified on the Mac

Runs AFTER parallel sessions have committed (cloud ones also pushed) to their branches. It does not care what the work is
(NVN, shaders, services, audio, loader, anything): it discovers the branches, verifies each on this Mac, lands what is sound,
reports what is not, and finishes with `/napple-latest`.

**Repo gotchas**
- The git repo is the OUTER folder `/Users/neto/Documents/Napple`. `Napple/.git` is a stale nested repo: never run `git` inside `Napple/` or with `git -C Napple`.
- `origin` is `git@github.com:erostalevi/NappleCloudRepo.git`; cloud sessions push `origin/claude/*` (and any branch the user told them to use). Check each cloud branch's
  paths: if they have no `Napple/` prefix (the cloud repo was synced from the `Napple/` folder), bring them in under that prefix instead of merging blindly
  (`git read-tree`/subtree merge into the scratch worktree) and tell the user what you did.
- Builds run in `$TMPDIR` (File Provider xattrs on ~/Documents break codesign); `napple_latest.sh` already handles this, so reuse its targets rather than inventing commands.

**Arguments (optional):** an orchestration branch (`orchestrate/<batch>`) and/or orchestration names. With a scope, integrate only those items (and whatever they `depends_on`), and ignore other branches.
Without arguments, integrate everything found in the inventory. This skill may run inside a background agent started by `/napple-orchestrate`: then the final report is your final message, and you must not ask the user questions
(a decision you cannot make is a Heavy/not-landed item with the question written in its report).

This skill changes git (merges into local `main`). It never pushes, never force-anything, never deletes or rewrites a session's branch. The user pushes `main`.

## 0. Preconditions (stop and ask if any fails)
- `mcp__ccd_session_mgmt__list_sessions` (linked and general; where it exists) and, for orchestrated items, `session`/`status` in `orchestrations/<name>.md` plus `get_session` if available: no session is still working on these branches. An item whose file says `launched` but has no pushed branch or integrator file is not ready: leave it out and report it.
- `git status --short` is clean in the main checkout and in every worktree under `.claude/worktrees/` (untracked `.claude/worktrees/` itself is fine). Uncommitted work: stop and tell the user; do not stash it for them.
- `git fetch origin` is allowed (so cloud branches are visible). Nothing else touches the remote.

## 1. Inventory
**Orchestration files first.** If `orchestrations/*.md` exist (on `main` or on `origin/orchestrate/<batch>`), they define the set: for each (in scope) item take `branch`, `env`, `model`, `depends_on`, `status`,
and the `Acceptance` list. If `origin/orchestrate/<batch>` is not yet in `main`, merge it into `main` first (`--no-ff`, `integrate: orchestration plan <batch>`; it only adds files under `orchestrations/`, and session branches never edit them, so later merges stay conflict-free).
`depends_on` sets the merge order; the integrator file's `orchestration:` field links the two. A branch whose orchestration file says `launched` but has no integrator file is "not handed off": do not merge it, report it. Acceptance items feed the checks in step 2.
List every branch with commits not in `main` (local, worktree branches, `origin/*`) with commit count, last commit time and head. Each parallel session (see `/ura-napple-parallel`)
leaves an **integrator file** `integrators/<branch>.md` (front matter: branch, base, head, env, areas, status) on its branch; also accept `Napple/integrators/` if the layout has the prefix.
Read it with `git show <branch>:integrators/<file>.md` (it is not in `main` yet). It is the primary source: its "Unbuilt / untested list", "How to integrate" (order constraints, conflict hotspots, build commands,
Linux-only calls to port, code to leave as C), "How to test" (checks with pass/fail criteria), "Fix policy hints" and "Needs from other areas". Also read the branch's `docs/archive/changes/` note.
- A branch with no integrator file: flag it, reconstruct what you can from its change note and diff, and treat it as higher risk (never skip it silently).
- `head` in the file differing from the branch's real head means commits landed after the hand-off: note it and treat those commits as undeclared.
- `status: blocked` or "Needs from other areas" items that depend on another branch set the merge order (the depended-on branch lands first); a dependency on a Heavy/unlanded branch makes this one Heavy too.
Table: branch | env (mac / linux-cloud) | areas touched (from `git diff --stat main...<branch>`) | declared-untested items | order constraints.
Propose a merge order: independent and low-risk first (docs, Rust crates, audio/input), shared runtime next, `gfx` last. Ask only if the order is not obvious.

## 2. Per-branch verification (scratch worktree, never the main checkout)
For each branch in order: `git worktree add <scratchpad>/integ-<name> -b integ/<name> main`, `git merge --no-ff <branch>` inside it, then build and test
the areas the diff touches, using the same targets as `napple_latest.sh`:
`gfx`, `audio`, `input`, `frontend`, `runtime`, `launcher` (make), the Rust workspace (`cargo build --release` / `cargo test`), `swift test` for Studio. Then the checks in the branch's integrator file ("How to integrate" build commands, "How to test" steps) that need no human, in the order given, plus the unbuilt/untested list first
(headless scripted runs for the games in `Testing Material`; see `docs/reference/tooling.md` and `work/` for the existing scripted-run tooling; never ask the user to play unless a check cannot be scripted).
If the file is wrong or incomplete (command fails for reasons unrelated to the code, missing step), correct your own run and record the correction in the file's result section (step 4).
Use its "Fix policy hints", but the Small-fix/Heavy limits below still decide.
Classify:
- **Clean**: builds, tests pass.
- **Small fix**: plain macOS problems (missing include, Linux-only header or function, signature mismatch, typo, path assumption). Fix in the scratch worktree, one commit each, message starting
  `integrate:`. At most ~10 per branch, none that change behaviour or design.
- **Heavy**: more than that, or anything needing a design decision, a rewrite of the author's logic, or runtime failures not clearly from the build. Stop on that branch, leave it out of `main`,
  record the first useful log lines, move on. Cloud work that was never built on macOS is expected to land here sometimes; that is a report, not a failure of this skill.
Merge conflicts: resolve only mechanical ones (adjacent edits, both sides appended to the same list or doc). A semantic conflict is Heavy.

## 3. Mac-native pass (Clean and Small-fix branches only)
Move code written as portable Linux-style C to the macOS-native equivalent only where it is a real gain or needed for correctness: `epoll` to `kqueue`/GCD, `/proc` to `sysctl`/Mach calls,
`clock_gettime`/`nanosleep` pacing to `mach_absolute_time`/dispatch timers, `getrandom` to `arc4random_buf`, `memfd`/`mremap` to Mach VM calls, hand-rolled thread pools to GCD,
ALSA/pulse-style audio output to CoreAudio/AudioQueue, Linux-only `#ifdef`s. LEAVE alone: pure logic (parsers, IPC/svc tables, DSP maths, state machines), POSIX code that works on macOS, the Maxwell
translator (portable on purpose), Rust crates that already build, and anything with a passing test and no platform dependence. In `gfx` never move away from Metal. When in doubt leave it as C and say so.
Each port is its own commit (`integrate: native <what>`) with a test, then re-run step 2's checks.

## 4. Land on main
For each Clean or Small-fix branch in order: merge `integ/<name>` into local `main` (`--no-ff`, message names the branch and what was fixed), then re-run the affected areas' tests on `main` after EACH merge so a later
branch cannot silently break an earlier one. If a merge breaks tests, revert that merge commit and reclassify the branch Heavy.

**Close the integrator file as part of landing**: right after a branch's merge into `main` is green, move its integrator file out of the open folder, so `main` never carries open integrator files:
```bash
mkdir -p docs/process/archive/integrators
git mv integrators/<file>.md docs/process/archive/integrators/<file>.md
```
Append a `## Integration result` section to the moved file (date, merged commit, outcome Clean / Small fix, the `integrate:` fixes made, native ports made, C left alone on purpose, test results, corrections to the file's instructions),
set `status: integrated` in its front matter, and commit on `main` as `integrate: close <branch>`. A Heavy (unlanded) branch is never merged, so its file never reaches `main`; its report is in step 6, and the file stays on its own branch.
**Do the same for the orchestration file** of an orchestrated branch: in the same `integrate: close <branch>` commit, `git mv orchestrations/<name>.md docs/process/archive/orchestrations/<name>.md`, set `status: integrated`, and fill its `## Result` section
(outcome, merged commit, link to `docs/process/archive/integrators/<file>.md`). For a Heavy (unlanded) item the orchestration file stays in `orchestrations/`: set `status: blocked` and write the reason and report location into its `## Result`, in a commit on `main`
(`integrate: mark <name> blocked`; this commit touches only that file). `orchestrations/TEMPLATE.md` and `README.md` are never moved.
Afterwards verify `ls integrators/*.md` on `main` shows nothing except `TEMPLATE.md` (never move, edit or delete `TEMPLATE.md`; `docs/process/archive/integrators/` holds the closed ones), and that `orchestrations/*.md` holds only `README.md`, `TEMPLATE.md`, optional `backlog.md` and the `blocked` items. Then remove the scratch worktrees and `integ/*` branches (session branches stay).
Leave `docs/process/archive/integrators/` tracked: it is the history of what was integrated and how.

## 5. Final build
Run the `napple-latest` skill (survey, clean rebuild, tests, `~/Desktop/NappleLATEST`). Then a headless boot of LAS and SMO from that build. Never call anything "played" or "working in a game" beyond what
the scripted runs and tests actually covered.

## 6. Report
Short and honest:
- **Landed on main**: each branch, commit range, small fixes made, native ports made (and the C left alone on purpose).
- **Not landed (Heavy)**: branch, why, first useful log lines, the decision a human or the author session must make. Nothing was force-fixed. Its integrator file stays on its branch (not on `main`).
- **Orchestration files**: which moved to `docs/process/archive/orchestrations/`, which are `blocked` (with the decision needed), which were not handed off. Include a ready-to-use list of follow-up items (Needs from other areas, Heavy leftovers, unverified Mac checks) for the orchestrator's next batch.
- **Integrator files**: which were moved to `docs/process/archive/integrators/`, which branches had none, and confirmation that `main` has no open files in `integrators/`.
- **Still unverified**: declared-untested items no run here covered.
- Test results, the NappleLATEST path and `BUILD_INFO.txt`, and a reminder that `main` is local and unpushed.
- Suggest `/napple-plan` afterwards (it runs `/percent-analysis` and compares with the last snapshot); only landed and verified work should raise the coverage number.
