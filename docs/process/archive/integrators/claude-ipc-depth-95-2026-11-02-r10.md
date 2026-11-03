---
branch: claude/ipc-depth-95-2026-11-02-r10
orchestration: ipc-depth-95
base: 12c6eab (+ merge of local claude/ipc-depth-cloud-2026-11-02-r9 at 3b5e0b7)
head: see git log (last commit of the branch)
env: mac
date: 2026-11-02
areas: [runtime/services, runtime/tests]
status: integrated
---

# claude/ipc-depth-95-2026-11-02-r10: IPC command depth 92% -> 98.0%

## Summary
scan.py IPC depth: 1079 impl / 246 stub of 1325 ref rows (+175/1) = 91.8% -> 1264 / 61 (+233/1) = 98.01% exact (scan prints 98%), missing 0.
Work: friend:u (43 rows, friend_core.c), ssl (25), nifm/bsd/nsd/sfdnsres (35), fs/account/olsc (31), applet/btm/lm/ovln/aoc (16),
bcat/npns/ldn (20), plus prepo ReadAll* read-back, lm accessor, aoc IAsyncData, nsd GetSettingUrl, SetLcdBacklighOffEnabled via lbl (11).
Five parts were done by subagents in worktrees, branches claude/ipc-depth-95-r10-{net,ssl,fsacc,applet,local} (merged here; per-row reports in
the session scratchpad part-*.md, summarised below). 62 rows stay IPC_STUB, each with its reason in the table comment.
Main (e412e99) merged; change note docs/archive/changes/2026-11-02-ipc-depth-95-2026-11-02-r10.md and a runtime-services.md section written. Merge conflicts: hz_result.h (both appends kept) and account.c (IManagerForApplication 2/3/4 take main's napple-online id-token rows; this branch's 160/170 stay).

## Decisions
- Merged local claude/ipc-depth-cloud-2026-11-02-r9 first (it converts rows in push.c/misc.c/bcat.c/vi.c); integrate it before this branch.
- Friend rows: cache rows need a linked account (provider account_info), sync rows also ONLINE_CAP_FRIENDS + network; offline answers unchanged.
  online-friends (r10) owns friend forwarding: expect conflicts in offline.c friend block (friend_service_cmds) — keep both, its provider calls win.
- Kept stubs where no public layout exists or a provider hook is missing (listed in table comments).

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| scan depth >= 98% | tested | `python3 -I .claude/skills/percent-analysis/scan.py`: 1264 impl / 61 stub ref, 237/1 unfixtured (98.02%) on the merged head |
| unit tests | tested | `make -C runtime GFX=napple test test-services test-ipc-reference` rc 0 after the main merge (84 binaries, 0 failures), ipc_reference missing 0 errors 0 (new: test_svc_friend_r10, _ssl_r10, _net_r10, _sfdnsres_r10, _fsacc_r10*, _applet_r10, _misc_r10, _local_r10_*, _prepo_r10) |
| four-title boots | verified | final head (after the main merge): SMO, LAS, MK8D, ACNH, each offline and NAPPLE_NET=host, 60 s, private save roots, run lock: rc 0 all 8, missing_ipc [], unsupported_svcs [], only stubbed nvdrv:13 |

## How to integrate / test
1. Merge after ipc-depth-cloud. Conflict hotspots: runtime/src/services/offline.c (friend block vs online-friends), hz_result.h (appends: keep all).
2. Gate above; then four-title boots on the final head (offline and NAPPLE_NET=host), rc 0, missing_ipc [].
## Risks (check first)
- applet: ExecuteProgram / CreateApplication*AndRequestToStart now execv the runtime on macOS (restart of the same program); only tested via a hook.
- fs: fsp-srv 31/80-88/110/606/610/618 check main.npdm FS permission bits; a wrong bit mapping would refuse a title's call (all 8 boots pass).
- account 2/3/4/103/170 and olsc now return 0x1907C for an unlinked user; nsd app env type default 0 -> 1 (lp); HDCP state default 2 -> 0.
## Needs from other areas
- signals.c: honour IApplicationFunctions 124 EnableApplicationAllThreadDumpOnCrash (thread_dump_all on crash).
- OnlineProvider hooks for friend images/requests, TLS cert/cipher, ec tokens, application authentication (rows kept as stubs).

## Integration result
- Date 2026-11-02, merged into main as 5d0c5ec (head 98dc102, which already contained main e412e99 and ipc-depth-cloud). No conflicts with what landed
  since (online-race's nifm row, nvdrv-public-layouts, online-support-table). Outcome: **Clean** plus one doc commit (the change note's row in the r10
  change archive; it was linked only from runtime-services.md).
- `make -C runtime GFX=napple`: no new warnings. `make -C runtime GFX=napple test test-services test-ipc-reference`: rc 0, ipc_reference 111 / 1325 /
  missing 0 / errors 0. `scan.py`: IPC command depth **98%** (1264 impl / 61 stub reference rows, 237 / 1 unfixtured).
- Regression runs for the rows it changed in nifm / bsd / ssl / account (one job each under the run lock): MK8D LAN Play pair through napple-room:
  2,226 / 2,167 Pia unicasts, no error; MK8D online login through napple-online: Authentication + SecureConnection ok, no error; online pair: both
  matched into one gathering, joiner MatchMaking 41 + NAT traversal, joiner 2618-0502 (as before). Four-title 45 s boots offline and `NAPPLE_NET=host`:
  rc 0, notify-running, `missing_ipc []`, stubbed only `nvdrv:13`.
- No native ports. Not checked: the applet ExecuteProgram execv path on a title (Risks), FS permission bits beyond the four titles.
- online-friends (landed next) conflicted with this branch's friend rows in `offline.c`; see that file's result for the resolution.
