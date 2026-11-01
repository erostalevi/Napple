---
name: ipc-depth-sys
batch: 2026-11-01-r2
branch: claude/ipc-depth-sys-2026-11-01-r2
base: orchestrate/2026-11-01-r2
env: mac
model: claude-sonnet-5-5
depends_on: []
status: handed-off
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_afc5486b-71f2-45b6-b9f1-22a1a94901ac (reused sweep-small r1 session, sonnet)
launched_at: 2026-11-01
---

# ipc-depth-sys: IPC command reference lists + missing rows for fs, vi, time, account, ns and the other system services

## Goal
Same job as ipc-depth-hid (parallel) for the other half of the services: a command with no row stops the runtime (`runtime/src/ipc.c:381`). Write reference lists of application-reachable commands and add every missing row. Expected about +0.13 pts.

## Owned paths
`runtime/src/services/{fs.c,vi*.c,time*.c,account*.c,ns*.c,ncm*.c,pdm*.c,olsc*.c,bcat*.c,mii*.c,mii_img*.c,caps*.c,ldn*.c,offline*.c,online_core.c,sfdnsres*.c,misc.c,ngc*.c,ectx.c,lm.c,ovln.c,push.c,pl.c,sysconfig_core.c}` (pctl rows live in misc/offline: yours),
their `runtime/tests/test_svc_*.c`, `runtime/tests/ipc_reference/<interface>.txt` for these interfaces, `docs/archive/changes/2026-11-01-claude-ipc-depth-sys-2026-11-01-r2.md`. `runtime/Makefile`, `docs/architecture/runtime-services.md`: append only.

## Do not touch
`runtime/src/ipc.c` and `runtime/tests/test_ipc_reference.c` (ipc-depth-hid writes the runner), ipc-depth-hid's service files (hid, hidbus, irs, audio, audin, audrec, psm, lbl, apm, settings, setfd, spsm, fgm, btm), `applet*.c`, `nvdrv*.c`, `jpeg_core.c`, media files (`movie_audio_core.c`, `codec_headers_core.c`, `decoder_*`, `media_gfx_glue.c`), sched-applets paths, `orchestrations/`.

## Fixture format (shared with ipc-depth-hid)
`runtime/tests/ipc_reference/<service-or-interface>.txt`, one command per line: `id<TAB>name<TAB>min_fw<TAB>app_reachable(y|n)`, `#` comments, first line `# interface <name> (<source>)`. ipc-depth-hid writes the runner; until it is merged, check your fixtures with a small local script (do not commit a second runner).
Build lists from libnx `source/services/*.c` wrappers and switchbrew, layout facts only; copy no GPL code.

## Acceptance
1. One fixture per interface you own (fsp-srv + IFileSystem/IFile/IDirectory/IStorage, vi:u/m/s + IApplicationDisplayService, time + ISystemClock/ISteadyClock/ITimeZoneService, acc:u0 + IProfile/IManagerForApplication, ns/ncm/pdm/olsc/bcat/mii/caps/ldn/sfdnsres/nifm/friend/prepo/nsd/ssl/bsd offline sets, pl:u, lm, pctl, ...).
2. Every app-reachable command has a row: real behaviour or a documented safe default (offline services: the offline answer the SDK expects).
3. `make -C runtime test` + `test-services` green; four-title boots: no new refusals.

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data, locks, local-only, never ask).

## Result
