---
branch: claude/ipc-risk-mac-checks-2026-11-03-r11
orchestration: ipc-risk-mac-checks
base: 281f4d3483540816dea22517d4d3d9a263adfb67
head: 23fefed (this file's own commit follows)
env: mac
date: 2026-11-03
areas: [runtime/tests, tools/golden]
status: ready
---

# claude/ipc-risk-mac-checks-2026-11-03-r11: exercise the recent IPC risk rows on the real titles

## Summary
Added `runtime/tests/test_risk_title.c` (one TU including applet.c, account.c, fs.c) and the Makefile target `test-risk-title` (not part of `make test`: it needs the shared title data; a missing title is a SKIP). It covers: (1) the real `host_relaunch` execv: a forked child calls ExecuteProgram(RestartProgram) with a pushed user-channel storage; the exec'd copy of the test binary sees the new argv, pops the storage, GetPreviousProgramIndex = 0 and the hand-over variable consumed; (2) fs.c permission bits from the real main.npdm of all six build dirs of the four titles: own device + account saves open, RomFS (romfs.map in place) opens with non-empty size, and each gated mount (81/82/86/87/31/110) follows the npdm bits (all PermissionDenied for these retail titles); (3) acc with the NullProvider (no linked Napple profile): 103 and 170 answer 0x1907C, GetAccountId is the fixed local id. Item 4 (four-title boots) re-run, see below. Nothing in src/ changed.

## Files
runtime/tests/test_risk_title.c (new), runtime/Makefile (+8 lines appended), tools/golden/risk/README.md (new), this file.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| 1 execv restart | verified | `make -C runtime test-risk-title`: real execv, argv/channel/prev index checked |
| 2 npdm FS bits, four titles | verified | 6 builds, 158 checks 0 failed, skipped 0 |
| 3 unlinked acc 0x1907C | verified | same run |
| 4 four-title boots | verified | runtime-binary --headless --seconds 40-45 each under .run.lock, private save root: rc 0, missing_ipc [], unsupported_svcs [], last_stage notify-running, stubbed_ipc [nvdrv:13] on all four |

## Unbuilt / untested list
None.

## Findings
- SMO fs_perms = 0x4000000000000000 (only the Debug bit), LAS = 0, MK8D/ACNH = 0x...40000 (bit 18). No gated mount is granted to any of them: retail behaviour matches the switchbrew tables.
- Boots stub row: nvdrv:13 stubbed on all four titles (not missing, not mine; belongs to the nvdrv session).
- applet.c and fs.c both define static storage_destroy/storage_cmds: the test renames the applet ones with #define around its include (no source change).
- The run lock was not needed for the restart/fs/acc test (it starts no game); the boots used it.
- "restart ... new program args": host_relaunch re-execs with the process's own argv by design (no new args are passed by the guest); the test replaces argv in the child and proves execv preserves it.

## How to integrate
No ordering constraints. Hotspot: runtime/Makefile (appended block at the end). Build: make -C gfx frontend input audio libs, then `make -C runtime test-risk-title`.
Only macOS (uses _NSGetArgv).

## How to test
1. `make -C runtime test-risk-title` -> last line `{"test":"risk_title","checked":158,"failed":0,"skipped":0}`.
2. Boots: `lockf -k /Users/neto/NappleWork/.run.lock runtime/build/napple-runtime <data dir> --headless --seconds 40 --summary s.json --save-root <private>`; check counters.missing_ipc == [].

## Fix policy hints
Safe to fix: include/path typos. NAPPLE_WORK overrides the title data root.

## Needs from other areas
nvdrv:13 stub (nvdrv owner).
