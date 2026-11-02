---
name: ipc-reference-audit
batch: 2026-11-02-r4
branch: claude/ipc-reference-audit-2026-11-02-r4
base: orchestrate/2026-11-02-r4
env: mac
model: claude-opus-5-5
depends_on: []
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_afc5486b-71f2-45b6-b9f1-22a1a94901ac (reused, sonnet)
launched_at: 2026-11-02
---

# ipc-reference-audit: fix misnumbered IPC rows, nifm offline request result, more local stubs made real

## Owned paths
`runtime/src/services/` EXCEPT `applet*.c/.h` and `applet_*_core.*`; non-applet `runtime/tests/test_svc_*.c`; non-am fixtures in `runtime/tests/ipc_reference/` (corrections only, with a cited source); `docs/architecture/runtime-services.md` (rows in place + appended section); own change note.

## Do not touch
applet files (am-stubs-legal), `runtime/tests/test_ipc_reference.c`, `kernel.c`, `gfx/`, `audio/`, `orchestrations/`.

## Acceptance
1. Every "is named X, the reference says Y" note from `runtime/build/tests/test_ipc_reference runtime -v` resolved: renumber to the switchbrew/libnx id (update its test), rename (name-only), or correct the fixture with a source. Target 0 meaning-differs mismatches. Known list: pctl 1018/1035/1061/1062/1451, pdm:qry 8-10, ectx:aw 0/1, fsp-srv 30, IGeneralService 15-17, bsd 29-31, ISslConnection 19-21, ldn 100/101/104/402, prepo, olsc, miiimg, ISteadyClock 200, aoc 13, sfdnsres 8.
2. nifm IRequest GetResult / GetAppletInfo / GetAdditionalInfo / GetRequirement answer like an offline console; GetAppletInfo names the network-connection library applet per switchbrew (contract with am-stubs-legal).
3. >= 15 more local stubs real (fs.c 21, account.c, misc.c aoc rows, vi 3, btm 2, lm 1, ovln 1); the rest listed with reasons.
4. `make test`, `test-services`, `test-ipc-reference` green (missing 0); four-title 60 s boots.

## Starting points
`offline.c:335-420`, `runtime/build/tests/test_ipc_reference runtime -v`.

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data — rebuilt in r3 with the ordering fix, never overwrite it; locks; local-only; never ask). This is planned as the LAST reachable round (snapshot 2026-11-02-5b9cf3a, 98.3%, ceiling ~98.9-99.0%).

## Result

Integrated 2026-11-02: **Clean**, merged into main as 67b4dfc (branch head 44bcc51). test / test-services / test-ipc-reference green (missing 0, svc_nifm_audit 122/0); four-title boots rc 0, missing_ipc []. 65 of 67 reference notes resolved; the two applet.c notes went to the am-stubs-legal integration. nifm IRequest GetResult reached by SMO and ACNH at boot (offline answer, both keep running).
Integrator file: `docs/process/archive/integrators/claude-ipc-reference-audit-2026-11-02-r4.md`.
