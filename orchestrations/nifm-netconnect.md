---
name: nifm-netconnect
batch: 2026-11-02-r5
branch: claude/nifm-netconnect-2026-11-02-r5
base: orchestrate/2026-11-02-r5
env: mac
model: claude-opus-5-5
depends_on: []
status: launched
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_936fd989-d4c6-4f34-ab50-b47f38dba7ca (reused)
launched_at: 2026-11-02
---

# nifm-netconnect: nifm Cancel semantics + real NetConnect output so MK8D Online Play shows the console's network flow

## Goal
On a console the game calls Cancel before GetAppletInfo and the NetConnect screen still appears: Cancel must keep a rejected request's result/applet info (only an accepted request is released and reset). Our NetConnect output (`{u32 result; u32 pad}`) makes MK8D retry ~20 times: find the real output from evidence.

## Owned paths
`runtime/src/services/offline_core.c`, the nifm part of `offline.c`, `applet_lib_core.[ch]` (NetConnect handler/encoder), `runtime/tests/test_svc_nifm_audit.c`, NetConnect cases in `test_svc_applet_am.c`; filler: `fs.c` rows 15/16/606/618 and `vi.c` 2102 (only where switchbrew documents the layout) + tests; `docs/architecture/runtime-services.md` (append), own change note.

## Do not touch
`applet.c` beyond NetConnect registration, `gfx/`, `crates/`, `runtime/src/a32*`, `kernel.c`, ipc_reference fixtures, shared `NappleWork/games` (read-only), `orchestrations/`.

## Acceptance
1. Cancel on a rejected (Free) request keeps result + applet info; on an accepted one releases/resets; pin at `test_svc_nifm_audit.c:47` updated.
2. NetConnect output from evidence: the SDK code consuming it via `nn::nifm::HandleNetworkRequestResult` in an A64 sdk module (addresses noted); if firmware extraction works, the real netConnect applet (likely 0100000000001006 in `~/Desktop/switchfiles/Firmware.22.1.0.zip` with `Keys 22.1.0`; reuse the 0100000000001009 Mii Maker extraction tooling) confirms it. Decide from the same evidence whether GetAppletInfo's 0x1000 input list is filled.
3. **Verified**: MK8D update Online Play offline (`NAPPLE_INPUT_SCRIPT=15:A,19:DOWN,20.5:DOWN,22:A`, `NAPPLE_TRACE_IPC=all`): `CreateLibraryApplet id 0xf` once per attempt, no retry loop, the game ends the attempt cleanly (its own error or back to the menu), no crash.
4. `make -C runtime GFX=napple test test-services test-ipc-reference` green, missing 0.
5. Four-title 60 s boots (ACNH 90 s): rc 0, `missing_ipc []`, SMO/ACNH nifm calls unchanged.

## Starting points
Experiment patch `/private/tmp/claude-502/-Users-neto-napple/e4353971-a4ef-41e2-8ed2-7e4cf1f5933b/scratchpad/exp-nifm-cancel.patch`; trace in `docs/process/archive/integrators/claude-am-stubs-legal-2026-11-02-r4.md` (~line 137); SDK modules `<games>/<tid>/<build>/modules/sdk.patched.bin` (MK8D update is A64); switchbrew Network_Interface_services.

## Env notes
Read `orchestrations/ENV-MAC.md` (shared title data rebuilt after r4 — read-only, use private copies; locks; local-only; never ask). Final planned round (snapshot 2026-11-02-8389280, 98.9%).

## Result
