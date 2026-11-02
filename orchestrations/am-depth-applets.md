---
name: am-depth-applets
batch: 2026-11-01-r3
branch: claude/am-depth-applets-2026-11-01-r3
base: orchestrate/2026-11-01-r3
env: mac
model: claude-sonnet-5-5
depends_on: []
status: launched
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_3b3cd256-1668-4a2b-8542-c3387e1fbb05 (reused, opus)
launched_at: 2026-11-01
---

# am-depth-applets: IPC reference + rows for the am interfaces, offline web kinds, applets on titles, frame-based input scripts

## Goal
The am (applet manager) interfaces are the busiest an application calls and have no reference fixtures yet; finish the offline web document kinds; verify more applets on titles; make scripted input frame-based.

## Owned paths
`runtime/src/services/applet*.c/.h`, `applet_web_core.*`, `runtime/tests/test_svc_applet*.c`, NEW fixture files `runtime/tests/ipc_reference/<am interface>.txt` (new files only), `runtime/src/input_script.c` + its test, `docs/architecture/runtime-services.md` (append an am section only), own change note.

## Do not touch
Other service files (ipc-local-stubs), existing fixtures and `test_ipc_reference.c` (use `# alias`/`# table` lines; runner changes go as a note in the integrator file), kernel/thread files, `gfx/`, `tools/golden/routes.json`, `orchestrations/`.

## Acceptance
1. Switchbrew fixtures with app_reachable flags for IApplicationProxyService/IApplicationProxy, ICommonStateGetter, ISelfController, IWindowController, IAudioController, IDisplayController, ILibraryAppletCreator, ILibraryAppletAccessor, IStorageAccessor, IApplicationFunctions, IDebugFunctions (mind the existing exact-name `IStorage` fixture). Every `y` id has a row; runner reports missing 0. Layout facts only, no GPL code.
2. Web applet: offline document kinds legal information (title's LegalInformation content) and system data (firmware system data) resolve instead of failing (`applet_web_core.c:306`); `test_svc_applet_web` extended; verify host-side on a title like the manual if one has legal-information data.
3. Controller-support / error / player-select / album applet verified on one of the four titles if any opens one; otherwise document how each was tried.
4. `input_script` frame-based timing option with a test.
5. `make -C runtime test` + `test-services` green; four-title boots no regression.

## Starting points
`applet.c:1854-1937` (IPC_OBJECT names), `applet_web_core.c:290-325`, `docs/process/archive/integrators/claude-sched-applets-2026-11-01-r1.md` (swkbd/manual recipes), `docs/process/archive/integrators/claude-sweep-media-r2-2026-11-01-r2.md` (route + validation decision), `runtime/tests/ipc_reference/IAudioOut.txt` (format example).

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data, locks, local-only, never ask). Shared title data in `/Users/neto/NappleWork/games/` predates the rewriter ordering fix (acnh-race-atomics, being integrated); rebuilt data for it is in `/Users/neto/NappleWork/games-r3/` — use either; never overwrite either.

## Result
