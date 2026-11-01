---
name: ipc-depth-hid
batch: 2026-11-01-r2
branch: claude/ipc-depth-hid-2026-11-01-r2
base: orchestrate/2026-11-01-r2
env: mac
model: claude-opus-5-5
depends_on: []
status: planned
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: none
launched_at: none
---

# ipc-depth-hid: IPC command reference lists + missing rows for hid, audio and system-device services

## Goal
A command with no row stops the runtime (`runtime/src/ipc.c:381`), and the depth score only counts rows that exist. Build per-interface reference lists of application-reachable commands and add the missing rows for this package's services. You also define the fixture format and runner that ipc-depth-sys (parallel) uses. Expected about +0.12 pts and turns the depth estimate into a measurement.

## Owned paths
`runtime/src/services/{hid.c,hid_core.[ch],hidbus*.c,hidbus_core.h,irs*.c,irs_core.h,audio.c,audin*.c,audrec*.c,psm*.c,lbl*.c,apm*.c,settings*.c,setfd*.c,spsm.c,fgm.c,btm.c}`, `runtime/src/ipc.c`,
new `runtime/tests/test_ipc_reference.c` and `runtime/tests/ipc_reference/<interface>.txt` for these interfaces, their `runtime/tests/test_svc_*.c`, `docs/archive/changes/2026-11-01-claude-ipc-depth-hid-2026-11-01-r2.md`.
`runtime/Makefile` and `docs/architecture/runtime-services.md`: append only.

## Do not touch
`input/` (sweep-media-r2), the service files of ipc-depth-sys (fs, vi, time, account, ns, ncm, pdm, olsc, bcat, mii, caps, ldn, offline, sfdnsres, misc, ngc, ectx, lm, ovln, push, pl, pctl), `applet*.c`, `nvdrv*.c`, media files, the sched-applets paths (kernel/thread/dispatch..., frontend/src, tools/soak), `orchestrations/`.

## Fixture format (fixed now, ipc-depth-sys writes the same)
`runtime/tests/ipc_reference/<service-or-interface>.txt`, one command per line: `id<TAB>name<TAB>min_fw<TAB>app_reachable(y|n)`, `#` comments, first line `# interface <name> (<source>)`. The runner prints, per fixture, the app-reachable ids with no row (impl or documented safe default) and fails when any exist.
Build the lists from libnx wrappers and switchbrew as layout facts only (ids, names, firmware); copy no GPL code.

## Acceptance
1. Reference lists for this package's interfaces; `test_ipc_reference` runs every fixture in the directory (including ipc-depth-sys's once merged) and passes for yours.
2. Rows for the missing IHidServer commands, at least 71, 73-78, 136, 207-210, 212, 303-306, plus all other app-reachable missing ids in your interfaces, each with real behaviour or a documented safe default.
3. `audio.c` OpenAudioIn comment refreshed (mic is the default when permitted, silence otherwise).
4. `make -C runtime test` and `test-services` green; four-title boots: no new refusals, no regression.

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data, locks, local-only, never ask).

## Result
