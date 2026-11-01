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
|---|---|---|

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
