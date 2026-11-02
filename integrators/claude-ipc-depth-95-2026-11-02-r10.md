---
branch: claude/ipc-depth-95-2026-11-02-r10
orchestration: ipc-depth-95
base: 12c6eab (+ merge of local claude/ipc-depth-cloud-2026-11-02-r9 at 3b5e0b7)
head: see git log (commit after 59cd36f)
env: mac
date: 2026-11-02
areas: [runtime/services, runtime/tests]
status: ready-with-caveats
---

# claude/ipc-depth-95-2026-11-02-r10: IPC command depth 92% -> 98.0%

## Summary
scan.py IPC depth: 1079 impl / 246 stub of 1325 ref rows (+175/1) = 91.8% -> 1264 / 61 (+233/1) = 98.01% exact (scan prints 98%), missing 0.
Work: friend:u (43 rows, friend_core.c), ssl (25), nifm/bsd/nsd/sfdnsres (35), fs/account/olsc (31), applet/btm/lm/ovln/aoc (16),
bcat/npns/ldn (20), plus prepo ReadAll* read-back, lm accessor, aoc IAsyncData, nsd GetSettingUrl, SetLcdBacklighOffEnabled via lbl (11).
Five parts were done by subagents in worktrees, branches claude/ipc-depth-95-r10-{net,ssl,fsacc,applet,local} (merged here; per-row reports in
the session scratchpad part-*.md, summarised below). 62 rows stay IPC_STUB, each with its reason in the table comment.
NOT DONE (usage limit): main was NOT merged again before hand-off; no change note in docs/archive/changes and no runtime-services.md section.

## Decisions
- Merged local claude/ipc-depth-cloud-2026-11-02-r9 first (it converts rows in push.c/misc.c/bcat.c/vi.c); integrate it before this branch.
- Friend rows: cache rows need a linked account (provider account_info), sync rows also ONLINE_CAP_FRIENDS + network; offline answers unchanged.
  online-friends (r10) owns friend forwarding: expect conflicts in offline.c friend block (friend_service_cmds) — keep both, its provider calls win.
- Kept stubs where no public layout exists or a provider hook is missing (listed in table comments).

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| scan depth >= 98% | tested | `python3 -I .claude/skills/percent-analysis/scan.py`: 1264 impl / 61 stub ref, 233/1 unfixtured (98.01%) |
| unit tests | tested | `make -C runtime GFX=napple test test-services test-ipc-reference` rc 0, ipc_reference missing 0 errors 0 (new: test_svc_friend_r10, _ssl_r10, _net_r10, _sfdnsres_r10, _fsacc_r10*, _applet_r10, _misc_r10, _local_r10_*, _prepo_r10) |
| four-title boots | partial | ran at 47b5d15 (friend only): SMO, LAS, MK8D, ACNH offline + NAPPLE_NET=host, 60 s each, rc 0 all 8. NOT re-run on the final head |

## How to integrate / test
1. Merge after ipc-depth-cloud. Conflict hotspots: runtime/src/services/offline.c (friend block vs online-friends), hz_result.h (appends: keep all).
2. Gate above; then four-title boots on the final head (offline and NAPPLE_NET=host), rc 0, missing_ipc [].
## Risks (check first)
- applet: ExecuteProgram / CreateApplication*AndRequestToStart now execv the runtime on macOS (restart of the same program); only tested via a hook.
- fs: fsp-srv 31/80-88/110/606/610/618 check main.npdm FS permission bits; a wrong bit mapping would refuse a title's call (boots not re-run).
- account 2/3/4/103/170 and olsc now return 0x1907C for an unlinked user; nsd app env type default 0 -> 1 (lp); HDCP state default 2 -> 0.
## Needs from other areas
- signals.c: honour IApplicationFunctions 124 EnableApplicationAllThreadDumpOnCrash (thread_dump_all on crash).
- OnlineProvider hooks for friend images/requests, TLS cert/cipher, ec tokens, application authentication (rows kept as stubs).
