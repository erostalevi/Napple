---
branch: claude/ipc-local-stubs-2026-11-01-r3
orchestration: ipc-local-stubs
base: 84cb5a6
head: 4da694d
env: mac
date: 2026-11-01
areas: [runtime/services, audio]
status: integrated
---

# claude/ipc-local-stubs-2026-11-01-r3: 66 local documented-default IPC stubs become real behaviour; per-stream audio gain

## Summary
66 of the ~100 LOCAL documented-default `IPC_STUB` rows (target: 60) are now `IPC_IMPL` with real behaviour and a unit test each; `make -C runtime test-ipc-reference` reports `missing 0` and the stub count of the application-reachable rows went from 304 to 238.
- **fsp-srv (22)**: the RomFS as a read-only `IFileSystem` (2, 7, 9, 10; Control from `control.nacp`, Manual from `html/`), data storages by program id (201, 205), save lookups with a filter (67, 68), internal storage (64), commit ids (616, advanced by every commit), free space, SD card / access log state, image / custom storage directories, system data update notifier, `OperateRange` on files and storages (IFile 5/6, IStorage 5).
- **acc:u0 (4)** digest / thumbnails / TrySelectUserWithoutInteraction, **aoc:u (2)**, **vi (2)**, **caps:u (4) + album movie read streams (5)**, **time (4)**, **pdm:qry (5)**, **ILogger (2)**, **ngc:u (2) with the word lists read from the firmware NgWord archive (3043 words)**, **IAudioOut 12/13 (2) with a real per-stream gain in `audio/`**, and the local-state rows of ISslConnection (4), nsd:u (2), prepo:u (3).
- New portable module `romfs_read.[ch]` (RomFS directory / file table reader) used by fs.c and ngc.c.
- NOT done: IBtmUserCore 1/2 (the system's scan parameter values are unknown, they stay documented defaults; the output size was corrected from 0x10 to the documented 8 bytes), `ConvertScalingMode` and `GetIndirectLayerImageMap` (undocumented layouts), fsp-srv `OpenSaveDataMetaFile` / `ListAccessibleSaveDataOwnerId` / `GetAndClearErrorInfo` / rights-id rows (undocumented arguments), acc:u0 130/131 + IManagerForApplication 160 (open context, undocumented), aoc:u 300/301. Network-needing rows stay defaults (list in the change note).

## Files
`git diff --stat 84cb5a6..HEAD`: 35 files, +2421 / -107.
- New: `runtime/src/services/romfs_read.{c,h}`, `runtime/tests/romfs_fixture.h`, `runtime/tests/test_svc_{fs_local,lm,local_offline}.c`, `docs/archive/changes/2026-11-01-ipc-local-stubs-2026-11-01-r3.md`.
- Changed services: `fs.c` (+700), `caps.c`, `time.c`, `account.c`, `offline.c`, `pdm.c`, `ngc.c`, `ngc_core.[ch]` (word list capacity 4096 -> 8192, `ngc_words_add_ngword`), `misc.c`, `vi.c`, `lm.c`, `audio.c`, `btm.c`.
- Changed audio: `audio/src/audout.c`, `audio/include/napple_audio.h` (**outside the listed owned paths** `audio/src + audio/tests`: two declarations, `napple_audout_set_volume/get_volume`, are required by the runtime), `audio/tests/test_audout.c`.
- Changed tests: `test_svc_{account,aoc,audio,btm,caps,ngc,pctl,pdm,time,vi}.c` (extended; the r2 assertions that pinned the documented defaults of the converted rows were replaced by the real behaviour; `test_svc_pctl.c` aoc row expectations: 200 and 302 are now real).
- Shared file touched: `docs/architecture/runtime-services.md` (rows refreshed in place + one appended section).

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Acceptance 1: >= 60 rows become `IPC_IMPL` with real behaviour | tested | 66 rows, listed per interface in the change note; each row has a check in a unit test (see below) |
| Acceptance 2: `make -C runtime test` + `test-services` | tested | `make test test-services` -> rc 0, 69 suites all `"failed":0` (new: `svc_fs_local` 255 checks, `svc_lm` 17, `svc_local_offline` 49; extended: time 105, account 76, caps 193, ngc 39, aoc 71, pdm 33, vi 110, audio, btm, pctl) |
| `make -C runtime test-ipc-reference` | tested | `{"test":"ipc_reference","fixtures":99,"app_reachable":1116,"missing":0,"errors":0}` |
| `make -C audio test` | tested | all suites `"failed":0`; `test_audout` 527 checks incl. `t_volume` |
| Per-stream gain really scales the mix | tested | `audio/tests/test_audout.c` `t_volume`: gain 0.25 turns 0.5 samples into 0.125 on both channels, gain 0 is silence, clamps 0..1, NaN is 0, released tags and the played-sample count are unchanged; the runtime row reads the library's value back (`test_svc_audio.c`) |
| RomFS file system view | tested | `test_svc_fs_local.c`: synthetic RomFS (files, nested dir, listing with directory / file filters, reads cut at EOF, write attempts refused, malformed tables / loops / out-of-range offsets); no real title called these rows |
| NG words from the firmware | tested | `test_svc_ngc.c` loads the real `sysdata/0100000000000806.bin` of the Testing Material build: 3043 words, valid lengths, a loaded word is flagged by the list; synthetic archive for the parsing / precedence cases |
| Acceptance 3: four-title 60 s boots | verified (no regression) / no converted row hit | SMO, MK8D (ff1d1c05), LAS, ACNH: each rc 0, `last_stage notify-running`, `missing_ipc []`, `unsupported_svcs []`, `stubbed_ipc` only `nvdrv:13` (ACNH none). Runtime binary, 60 s headless, private copied `--save-root`, under `.run.lock`. None of the 66 converted rows was called by these titles in 60 s (they use fsp-srv 1/51/200/202/1005, IFileSystem 7/8, acc 1/2/4/100/101/150/160, ISystemClock, ITimeZoneService 0/4/100/101, aoc 2/3/5/7/11/12/50, IAudioOut 1/7/8), so the rows stay *tested*, not *verified* |
| Acceptance 4: runtime-services.md rows refreshed | done | fsp-srv, acc:u0, aoc:u, time, vi, caps, pdm, audio rows + a section "ipc-local-stubs" |

## Unbuilt / untested list (the integrator checks these first)
Nothing is unbuilt. Not exercised by any real title (unit tests only): every converted row (see the boot results below for the ones a title did hit). Argument layouts that are inferred rather than read from switchbrew are listed in the change note ("Honest notes on the evidence"): fsp-srv 64, 100, 612, 620; time 6 (name at 0, SteadyClockTimePoint at 0x28, 0x40 bytes); acc 110/111; aoc 200; caps movie streams 2001-2005; ILogger 2/3; SSL 34/35; nsd 15/63; prepo 20200; `GetProgramIndexForAccessLog` = {1, 0}; `IsExFatSupported` always 1; ngc Check2/Mask2 = Check/Mask.

## Findings
- **Bug fixed**: `IBtmUserCore::GetBleScanFilterParameter` wrote 0x10 zero bytes; the documented `BleAdvertisePacketParameter` is 8 bytes (switchbrew Bluetooth_Driver_services). Still a documented default.
- **Bug fixed (behaviour)**: the save `commit_id` in `SaveDataExtraData` was never updated; every successful `Commit` now advances it (monotonic, seeded from the clock), so `ReadSaveDataFileSystemExtraData*` and `GetSaveDataCommitId` agree.
- **Decision**: new source files need no Makefile change (`runtime/Makefile` is not mine): the sources are wildcarded. `tests/host`'s `HOST_FS` source list (Linux host unit tests of fs.c) lacks `romfs_read.c`; fs.c carries weak fallbacks for the five `romfs_img_*` functions so those tests still link (the RomFS file system view is then unavailable there). Adding `src/services/romfs_read.c` to `HOST_FS` would make it real on the host build.
- **Decision**: NG words: all lists of the archive (`common.txt`, `0..15.txt`) are loaded, not just the host language's (the file-number to language mapping is not documented; the console's filter is not language aware). Lines using regular-expression syntax beyond `.*word.*` with backslash escapes are skipped; the matching rule is unchanged (ASCII case-insensitive substring).
- **Decision**: Control / Manual content file systems are read-only views of host directories (Control via a symlink directory so the module files next to `control.nacp` stay unreachable); Code, Logo, Meta and Package answer "path not found" because the host has no NCAs.
- The r2 note's `docs/architecture/runtime-services.md` refresh request (fsp-srv / nifm / friend / acc / aoc rows) is done for fsp-srv, acc, aoc; nifm / friend are network and unchanged.
- Out of my area, not touched: none found.

## How to integrate
- No order constraints. Conflict hotspots: `runtime/src/services/fs.c` (other fs work), `offline.c`, `docs/architecture/runtime-services.md` (append-only section at the end + in-place row edits), `audio/include/napple_audio.h` (two declarations next to `napple_audout_set_manual`).
- Build: `make -C audio` (libnapple_audio.a with the gain), then `make -C runtime` (picks up `romfs_read.c` through the wildcard). Tests: `make -C runtime test test-services test-ipc-reference` and `make -C audio test`.
- Linux-only calls: none. Everything is portable C plus the existing runtime APIs (`statvfs`, `symlink`, `realpath` are POSIX).

## How to test
1. `make -C runtime test test-services` -> rc 0, every line `"failed":0`; `make -C runtime test-ipc-reference` -> `"missing":0`; `make -C audio test`.
2. Four-title boots: `lockf -k /Users/neto/NappleWork/.run.lock runtime/build/napple-runtime <data dir> --headless --seconds 60 --summary <json> --save-root <private copy>`; pass = rc 0, `missing_ipc []`, `last_stage notify-running`. (ACNH loads the 3043 firmware NG words only when it first opens `ngc:u`, which it did not do within 60 s.)
3. Regression guard: the converted fsp-srv rows share `IStorage` with `OpenDataStorageByCurrentProcess`; a mod overlay (`--mods`) or BKTR delta data dir must still serve the same bytes through the RomFS `IFileSystem` view (not covered by a title here).
4. Nothing needs a human.

## Fix policy hints
Safe to fix directly: result-code choices, the `HOST_FS` source list, sizes of zero outputs. Needs a decision: replacing any of the inferred layouts above once a title shows the real one; making Check2/Mask2 differ from Check/Mask (undocumented since 21.0.0).

## Needs from other areas
- `runtime/Makefile`: add `src/services/romfs_read.c` to `HOST_FS` (Linux host tests of fs.c).
- `docs/index.md`: link the change note (the docs checker lists it as an orphan, like the other batch notes).

## Integration result
- Date: 2026-11-02. Merged into main as 3ed04f6 (integ/ipc-local-stubs, branch head 05b0fab; file `head: 4da694d` + one commit that only updates this file). Outcome: **Small fix** (no conflicts; landed after acnh-race-atomics).
- `integrate:` fixes: 9a86f2d `runtime/Makefile`: `HOST_FS` gains `src/services/romfs_read.c` (this file's "Needs from other areas" / "Safe to fix"); `test_fs_savedata` 34 and `test_fs_journal` 17 rebuilt and green with the real RomFS reader linked. Native ports: none (portable C; `statvfs`/`symlink`/`realpath` are POSIX, left alone on purpose).
- Build (scratch worktree): gfx lib adapter, audio, input, frontend, runtime (GFX=napple) rc 0.
- Tests: `make -C audio test` rc 0 (`audout` 527 incl. `t_volume`); `make -C runtime test` / `test-services` rc 0, every suite `"failed":0` (`svc_fs_local` 255, `svc_lm` 17, `svc_local_offline` 49, `svc_ngc` 39 with the real firmware NgWord archive, `svc_audio` 22); `test-ipc-reference` `{"fixtures":99,"app_reachable":1116,"missing":0,"errors":0}`; `test-host` rc 0.
- Boots (runtime of the merged tree, rebuilt title data `/Users/neto/NappleWork/games-integ/` with the rewriter ordering fix, `soak.py --lockf .run.lock`, 60 s headless, private save roots): LAS ok 3531 frames, SMO ok 3548, MK8D update ok 3397, ACNH update ok 1781; cb=0 on all.
- Not covered here: the converted rows in a title (none called them), the mod overlay / BKTR regression guard (How to test 3).
- Still open: `docs/index.md` link for the change note (docs owner).
