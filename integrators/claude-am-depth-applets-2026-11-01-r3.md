---
branch: claude/am-depth-applets-2026-11-01-r3
orchestration: am-depth-applets
base: 84cb5a6 (orchestrate/2026-11-01-r3)
head: aa39557
env: mac
date: 2026-11-01
areas: [runtime/services/applet*, runtime/input_script, runtime/tests/ipc_reference (new am files), docs]
status: ready
---

# claude/am-depth-applets-2026-11-01-r3: am IPC reference + rows, offline web kinds, applets on titles, frame-based input scripts

## Summary
- 12 new switchbrew fixtures for the application-facing am interfaces. `test_ipc_reference` reports 111 fixtures, missing 0. While writing them I
  found and fixed **five misnumbered rows**: IApplicationFunctions 91-93 → 100-102, 110 → 123, 120 → 130 (120 is ExecuteProgram), 130 → 140;
  ILibraryAppletAccessor 50 → 60. I also added 41 rows.
- Offline web: legal-information and system-data document kinds now resolve. System data pages come from the firmware archives through a new
  RomFS extractor. Verified over a running SMO (the console's legal-lines page in the web sheet).
- Applets on titles: controller support verified on MK8D (Multiplayer → 2 players → the game continues to the 2-player character select); swkbd
  re-verified on LAS with a frame-timed script. Error, player select and album could not be raised on the four titles (how each was tried is in
  the change note).
- `NAPPLE_INPUT_SCRIPT` frame-based timing (`f<n>`, `NAPPLE_INPUT_SCRIPT_FRAMES=1`), with a host test; verified on LAS (presses at exact frames).
- Not done: a title's own legal-information pages (napple-build does not extract LegalInformation content; see Needs).

## Files
`git diff --stat 84cb5a6..HEAD`:
- `runtime/src/services/applet.c`, `runtime/src/services/applet_web_core.{c,h}`, `runtime/src/input_script.c`
- Tests: `runtime/tests/test_svc_applet.c`, `test_svc_applet_host.c` (ids updated to the fixed numbering), `test_svc_applet_web.c`, `runtime/tests/host/test_input_script.c` (new)
- New fixtures: `runtime/tests/ipc_reference/{appletOE,IApplicationProxy,ICommonStateGetter,ISelfController,IWindowController,IAudioController,IDisplayController,ILibraryAppletCreator,ILibraryAppletAccessor,IStorageAccessor,IApplicationFunctions,IDebugFunctions}.txt`
- Shared, append-only: `runtime/Makefile` (one host-test rule for `test_input_script` after `test_kernel_sched`; needed to run the input_script test), `docs/architecture/runtime-services.md` (am section)
- Docs: `docs/archive/changes/2026-11-01-claude-am-depth-applets-2026-11-01-r3.md` (new)
- Not touched: existing fixtures, `test_ipc_reference.c`, other service files, kernel files, `gfx/`, `tools/golden/routes.json`.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Acceptance 1: am fixtures, every y id has a row | verified | `runtime/build/tests/test_ipc_reference runtime`: `{"fixtures":111,"app_reachable":1325,"missing":0,"errors":0}`; `-v` notes only row 210 (switchbrew leaves it unnamed) |
| Acceptance 2: web legal information / system data | verified (system data on a title) / tested (legal) | `test_svc_applet_web` 219/219; SMO windowed `NAPPLE_OPEN_WEB_DOC=25:3:0100000000000804:legallines.htdocs/index.html`: `web document: shown, exit reason 0, last page file:///.../napple-web-sysdata/0100000000000804/legallines.htdocs/index.html` (sheet screenshot checked; not committed, it is firmware legal text) |
| Acceptance 3: controller-support on a title | verified | MK8D `f900:A,f1300:A,f1450:DOWN,f1550:A,f1800:A,...`: `CreateLibraryApplet id 0xc mode 0`, `controller support: confirmed with 2 player(s) of 1 connected (game asked 2..2 ...)`; frame 3000 shows the two-player character select |
| Acceptance 3: error / player select / album | not raised | MK8D Online Play opens `nifm:u` and stays on the menu (no library applet); ACNH/MK8D use the preselected user; no title opens the album. Unit tests only |
| Acceptance 4: input_script frame timing | verified | `test_input_script` 16/16; LAS 118 s: `input: script: press L at frame 5950 ...`, `press A at frame 6560` → `swkbd: entered "Frames"` |
| Acceptance 5: `make -C runtime test` / `test-services` / `test-host` | verified | all rc 0 (63 service suites) |
| Acceptance 5: four-title 60 s boots | verified | `soak.py run las/smo/mk8d-base/acnh-update --runs 1 --seconds 60`: ok ×4 (3547 / 3544 / 2828 / 1782 frames) |

## Unbuilt / untested list (the integrator checks these first)
None unbuilt. Not exercised by a title (unit-tested): legal-information resolution, the new IApplicationFunctions/ICommonStateGetter stub rows,
CreateHandleStorage (unit test only), CreateLibraryApplet id 3.

## Findings
- The five misnumbered am rows were live bugs. A title calling ExecuteProgram (IApplicationFunctions 120) got an event handle as if it had asked for
  the GPU error event; one asking for the GPU error event (130) got the friend-invitation event; the copyright framebuffer calls (100-102) had no row
  (a stop). Tests updated to the switchbrew/libnx ids.
- MK8D's offline Online Play goes through nifm (`nifm:u`); the network error UI that a console shows comes from nifm's request result, not from the
  game creating the error applet. That is the nifm owner's path (not touched).
- Firmware system data `0100000000000804` = the console's legal lines (`legallines.htdocs`, `safe.htdocs`); `0100000000000803` = browser error templates.
- MK8D's 60 s boot presented fewer frames than in earlier rounds (2828 vs ~3300) on a busy shared Mac; same menus reached, no failure. Recheck when the
  machine is idle if the number matters.
- Decision (user away): reachability flags lean to `y` only for commands an application API reaches. System-applet services are `n` with a reason, so a
  missing row there is not a guest crash for applications.

## How to integrate
- No order constraints. Conflict hotspots: `runtime/Makefile` (the appended host-test rule; another branch appending in the same place: keep both),
  `docs/architecture/runtime-services.md` (appended section). `applet.c` is large but only this branch's area.
- If ipc-local-stubs (same batch) also touched am interfaces through `# alias`/`# table`, rerun `test_ipc_reference` after both land.
- Build: the usual `make -C runtime GFX=napple`; no data rebuild needed.

## How to test
1. `make -C runtime GFX=napple test` (look for `"missing":0`), `make -C runtime GFX=napple test-services`, `make -C runtime test-host` (`input_script` 16/16).
2. Controller support: MK8D headless 60 s, `NAPPLE_INPUT_SCRIPT="f900:A,f1300:A,f1450:DOWN,f1550:A,f1800:A,f2000:A,f2200:A,f2400:A"`: `CreateLibraryApplet id 0xc` and the 2-player answer in the log.
3. System data page: SMO windowed 45 s, `NAPPLE_OPEN_WEB_DOC=25:3:0100000000000804:legallines.htdocs/index.html` (+ `NAPPLE_SHEET_AUTO=confirm:4000 NAPPLE_SHEET_SHOT=<dir>` for a picture): `web document: shown`.
4. Frame script: LAS fresh save, 118 s, `NAPPLE_SWKBD_ANSWER=Frames NAPPLE_INPUT_SCRIPT="f5950:L+R:f40,f6200:A,f6380:A,f6560:A,f6740:A,f6920:A"`: presses logged at those frames, `swkbd: entered "Frames"`.
5. Regression: four-title 60 s boots ok.

## Fix policy hints
- A fixture flag a reviewer disagrees with is a one-character change (y/n) plus, for y, a row; safe to adjust directly.
- Renumbered rows: do not revert to the old ids (they disagree with switchbrew and libnx).

## Needs from other areas
- napple-build: extract each title's LegalInformation content (CNMT content type 5) to `<data-dir>/legal/`, like the HtmlDocument to `html/`, so legal
  information pages of titles resolve (the runtime side is ready).
- nifm (ipc-local-stubs / nifm owner): `IRequest` result / applet info for the offline case, so Online Play in MK8D raises the network error UI as on a console.
- `test_ipc_reference.c` runner (not changed, per the assignment): none needed.
