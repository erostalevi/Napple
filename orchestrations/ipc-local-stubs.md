---
name: ipc-local-stubs
batch: 2026-11-01-r3
branch: claude/ipc-local-stubs-2026-11-01-r3
base: orchestrate/2026-11-01-r3
env: mac
model: claude-opus-5-5
depends_on: []
status: launched
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_afc5486b-71f2-45b6-b9f1-22a1a94901ac (reused, sonnet)
launched_at: 2026-11-01
---

# ipc-local-stubs: make ~100 local documented-default IPC stubs real; per-stream audio gain

## Goal
Round 2 gave every app-reachable command a row, ~100 of them documented-default `IPC_STUB`s for LOCAL services that can have real behaviour. scan.py now measures IPC depth from `runtime/tests/ipc_reference/` (impl 1 / stub 0.5): each converted row is ~+0.0017 pts (+0.10..0.17 total).

## Owned paths
`runtime/src/services/` EXCEPT `applet*.c/.h` and `applet_web_core.*` (incl. `sys_default.h`, `fs.c`, `account*.c`, `misc.c`, `vi*.c`, `caps*.c`, `pdm*.c`, `time*.c`, `lm.c`, `ngc*.c`, `audio.c`, `btm.c`, `romfs_map.*`), their `runtime/tests/test_svc_*.c` (not applet ones), `audio/src/` + `audio/tests/` (per-stream gain), the `app_reachable` flags in the existing sys fixtures you own, `docs/architecture/runtime-services.md` (refresh fsp-srv/nifm/friend/acc/aoc rows in place only), own change note.

## Do not touch
`applet*.c`, `applet_web_core.*` (am-depth-applets); `runtime/src/{kernel.c,thread.c,threads.c,kcode.c,signals.c,hle_stack.c}`, `crates/napple-rewrite/`, `tools/soak/` (being integrated); `runtime/tests/test_ipc_reference.c`, `gfx/`, `input/`, `runtime/Makefile`, `orchestrations/`.

## Acceptance
1. >= 60 of the ~100 local documented-default `IPC_STUB` rows become `IPC_IMPL` with real behaviour from the built title data/keys/firmware: fsp-srv (41: OpenFileSystemWithId family by path/program id, OpenDataStorageByProgramId/DataId for DLC and system data, save-data info readers), IFile/IFileSystem/IStorage (2/2/1), acc:u0/IManagerForApplication (9/6, local profile), aoc:u (7, from the built DLC list), IApplicationDisplayService (5), caps:u/album session (4/5), time:u/ISystemClock/ITimeZoneService (1/1/2), pdm:qry (5), ILogger (3), ngc:u (2, NG-word system data from firmware), IAudioOut 12/13 (SetAudioOutVolume/GetAudioOutVolume with a real per-stream gain in `audio/`), IBtmUserCore (2). Network-needing rows stay documented defaults (list them).
2. Unit test per converted row; `make -C runtime test`, `test-services`, `test-ipc-reference` (missing 0), `make -C audio test` green.
3. Four-title 60 s boots (private `--save-root`, run lock): `missing_ipc []`, no regression; any converted row a title calls reported as verified.
4. runtime-services.md rows refreshed.

## Starting points
`docs/process/archive/integrators/claude-ipc-depth-sys-2026-11-01-r2.md` (Fix policy hints), `sys_default.h`, `make -C runtime test-ipc-reference`, `docs/process/archive/integrators/claude-ipc-depth-hid-2026-11-01-r2.md` (Needs: audio gain).

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data, locks, local-only, never ask). Shared title data in `/Users/neto/NappleWork/games/` predates the rewriter ordering fix (acnh-race-atomics, being integrated); rebuilt data for it is in `/Users/neto/NappleWork/games-r3/` — use either; never overwrite either.

## Result
