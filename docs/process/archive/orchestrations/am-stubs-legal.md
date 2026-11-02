---
name: am-stubs-legal
batch: 2026-11-02-r4
branch: claude/am-stubs-legal-2026-11-02-r4
base: orchestrate/2026-11-02-r4
env: mac
model: claude-sonnet-5-5
depends_on: []
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_936fd989-d4c6-4f34-ab50-b47f38dba7ca (reused, opus)
launched_at: 2026-11-02
---

# am-stubs-legal: title legal pages, am stub rows made real, offline network-connection applet

## Owned paths
`runtime/src/services/applet*.c/.h`, `applet_*_core.*`, `runtime/tests/test_svc_applet*.c`, am fixtures, `crates/napple-build/` (LegalInformation extraction), own change note.

## Do not touch
`offline.c` (nifm: ipc-reference-audit), `kernel.c`, `crates/napple-rewrite/`, shared `games/` (rebuild one title privately), `orchestrations/`.

## Acceptance
1. napple-build extracts LegalInformation content (CNMT content type 5) to `<data-dir>/legal/`, cargo test.
2. A title's own legal-information page opens in the web sheet, verified on one of the four titles.
3. >= 12 am stub rows with documented behaviour become real (e.g. RequestToShutdown / RequestExitApplicationAndTryPopMessage as clean exit, SaveCurrentScreenshot / TakeScreenShotOfOwnLayerEx via capture, SetRecordVolumeMuted, GetLaunchRequiredVersion, GetLastApplicationExitReason); rest listed.
4. Network-connection library applet completes offline with the console's "not connected" result, test (MK8D Online Play verified at integration once both r4 runtime packages land).

## Starting points
`crates/napple-build/src/html.rs`, `pipeline.rs:658`, `applet.c:1940-1998`, `applet_web_core.c`.

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data — rebuilt in r3 with the ordering fix, never overwrite it; locks; local-only; never ask). This is planned as the LAST reachable round (snapshot 2026-11-02-5b9cf3a, 98.3%, ceiling ~98.9-99.0%).

## Result

Integrated 2026-11-02: **Small fix**, merged into main as f73b8ec (branch head cb539e3); two name-only `integrate:` commits in applet.c close ipc-reference-audit's two leftover reference notes. All applet suites, ipc_reference (missing 0) and `cargo test -p napple-build` green; SaveCurrentScreenshot verified on LAS; four-title boots rc 0. Acceptance 4's title check (MK8D Online Play -> NetConnect) is **not reached** on main: MK8D cancels the nifm request before GetAppletInfo and Cancel clears the result (pinned by test_svc_nifm_audit). With a rejected request keeping its result (experiment, not landed) MK8D launches NetConnect, retries 20 times, then shows "A communication error has occurred." Follow-up needed: nifm Cancel semantics + NetConnect output layout.
Integrator file: `docs/process/archive/integrators/claude-am-stubs-legal-2026-11-02-r4.md`.
