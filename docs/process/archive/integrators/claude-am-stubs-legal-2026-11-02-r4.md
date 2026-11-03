---
branch: claude/am-stubs-legal-2026-11-02-r4
orchestration: am-stubs-legal
base: 90d3e3c (orchestrate/2026-11-02-r4)
head: see `git log -1` (code at b0723bd; docs commits on top)
env: mac
date: 2026-11-02
areas: [crates/napple-build, runtime/services/applet]
status: integrated
---

# claude/am-stubs-legal-2026-11-02-r4: title legal pages, am stub rows made real, offline network-connection applet

## Summary
- **napple-build** extracts each title's LegalInformation content (CNMT content type 5) to `<data-dir>/legal/`, next to the manual's `html/`. Unit and
  e2e cargo tests cover it.
- **Legal page verified:** LAS's own IP-notices page opens in the web sheet. Run on a private rebuild in `$TMPDIR`; the shared
  `NappleWork/games` was not touched.
- **13 am stub rows made real:**
  - IApplicationFunctions 70 / 71 / 310: the ExitRequested message, then a clean stop after a grace period.
  - IApplicationFunctions 180 / 181: launch-required version, persisted per title and only ever raised.
  - IApplicationFunctions 110 / 111: play statistics for the running title.
  - IApplicationFunctions 34 / 36: the full-license answers.
  - ISelfController 120 SaveCurrentScreenshot: the current frame into the caps album, **verified on LAS**.
  - IDisplayController 28 TakeScreenShotOfOwnLayerEx.
  - ICommonStateGetter 1004 / 1005: the panel's real values.
- **Real capture source:** the capture slots now read the real last frame. Before this, nothing registered a source, so every capture image was black.
- **NetConnect (0x0f)** finishes normally and returns the "not connected" result (2110-1111) instead of being unimplemented.
- Not done: MK8D Online Play has not reached NetConnect here, because nifm's GetAppletInfo (`offline.c`, not mine) is still a stub. The
  orchestration plans that check at integration.

## Files
- `crates/napple-build/src/html.rs`: `read_document` / `write_document`, plus `read_legal` / `write_legal` and `CTYPE_LEGAL`.
- `crates/napple-build/src/pipeline.rs`: the legal loop and the write.
- `crates/napple-build/tests/support/mod.rs`: a LegalInformation NCA in the html fixture.
- `crates/napple-build/tests/e2e.rs`: the `legal/` assertions.
- `runtime/src/services/applet.c`: the handlers above, the host capture registration, the NetConnect registration, the `NAPPLE_DEBUG_SAVE_SCREENSHOT` aid.
- `applet_core.h`: state fields.
- `applet_album_core.[ch]`: `applet_album_save_rgba`, `applet_album_source`.
- `applet_lib_core.[ch]`: the NetConnect handler.
- New `applet_capture_host.c`.
- New test `runtime/tests/test_svc_applet_am.c`.
- Docs:
  - new `docs/archive/changes/2026-11-02-am-stubs-legal-2026-11-02-r4.md`;
  - appended a section to `docs/architecture/runtime-services.md`;
  - `docs/reference/environment-variables.md`: two rows added (`NAPPLE_EXIT_GRACE_MS`, `NAPPLE_DEBUG_SAVE_SCREENSHOT`).

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Acceptance 1: LegalInformation -> `<data-dir>/legal/`, cargo test | tested + verified on a real title | `cargo test --release -p napple-build`: all suites ok (lib `html::tests::write_legal_goes_under_legal`; e2e `html_document_written_next_to_romfs_map` checks `legal/legalinfo.xml`, `legal/ipnotices.htdocs/index.html`, a nested image, no `legal/` without the content). Private LAS build (`napple build … --data-only -o $TMPDIR/am-legal/games`): `legal/` = `legalinfo.xml`, `ipnotices.htdocs/`, `support.htdocs/`, `important.htdocs/` |
| Acceptance 2: a title's legal page in the web sheet | **verified** (LAS) | windowed, `NAPPLE_OPEN_WEB_DOC=20:2:0:ipnotices.htdocs/index.html NAPPLE_SHEET_AUTO=confirm:5000 NAPPLE_SHEET_SHOT=<dir>`: `web document: shown, exit reason 0, last page file:///…/legal/ipnotices.htdocs/index.html`; sheet screenshot = LAS's ©1993-2019 Nintendo / GREZZO notice page (shared notes folder `am-stubs-legal-evidence/`) |
| Acceptance 3: >= 12 am rows made real | tested (13 rows); SaveCurrentScreenshot **verified** | `test_svc_applet_am` 67 checks, 0 failed. LAS `NAPPLE_DEBUG_SAVE_SCREENSHOT=12:<file>`: `SaveCurrentScreenshot: saved to the album (115030-byte JPEG)`, and the JPEG is the storm frame on screen |
| Acceptance 4: NetConnect offline with "not connected" | tested | `test_svc_applet_am`: CreateLibraryApplet(0x0f), CommonArguments and argument pushed, Start, completed, GetResult 0, output 8 bytes = `HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED`. MK8D Online Play: not reached (nifm GetAppletInfo is a stub in `offline.c`) |
| Suites | tested | `make -C runtime test test-services` rc 0: `ipc_reference` 111 fixtures / missing 0; applet 116, applet_ae 52, applet_album 63, applet_am 67, applet_host 539, applet_libs 263, applet_web 219 |
| Regression | verified | SMO / MK8D / ACNH / LAS 60 s headless with golden inputs, private saves: exit 0, no crash (3539 / 3541 / 1778 / 3380 frames). None calls a command changed here in that window |

## Unbuilt / untested list (the integrator checks these first)
- Nothing unbuilt. The `#else` (non-Apple) branch of `applet_capture_host.c` is a one-line no-op, never compiled here.
- The exit-request path, the launch-required version, play statistics and TakeScreenShotOfOwnLayerEx are unit-tested only: no test title calls them.

## Findings
- Every capture image (IDisplayController Get*CaptureImage, TakeScreenShotOfOwnLayer) was black in real runs, because nothing registered a capture
  frame source. `applet_capture_host.c` now registers the gfx readback. Titles that read capture images, e.g. for a pause-menu background, now see
  the game's frame.
- Decision: the exit requests send ExitRequested and wait a grace period before stopping, rather than stopping at once. A console tells the
  application to leave first, and a title that handles the message exits through its own path (main.c's finalizer exit).
- Decision: the NetConnect output is `{u32 result; u32 pad}` with nifm's offline GetResult code, so the applet and nifm agree. The layout is
  [unverified]. If the SDK expects another layout, only `applet_netconnect_encode` changes.
- Decision: 45 / 46 / 130 (layer separation, recording composition, record-volume mute) keep their value but stay IPC_STUB. The host has no game play
  recording or separable layers they could act on, so I did not count them as real.
- Panel values 137 x 77 mm are the original 6.2" LCD (16:9). An OLED model's 7" panel would differ; the host reports the original model.

### Rows still stubs, and why
| Row | Reason |
|---|---|
| ICommonStateGetter 52 SetLcdBacklighOffEnabled | no backlight to control on the host |
| ICommonStateGetter 62 GetHdcpAuthenticationState, IApplicationFunctions 170 SetHdcpAuthenticationActivated | HDCP state enum unverified |
| ICommonStateGetter 80 PerformSystemButtonPressingIfInFocus | SystemButtonType numbering unverified |
| ICommonStateGetter 100 SetHandlingHomeButtonShortPressedEnabled | the HOME hotkey is frontend-owned |
| ICommonStateGetter 130 EnableStartupLogoDisappearedMessage | message id unknown |
| ICommonStateGetter 1003 GetVrLaboGoggleViewport | Labo viewport unknown |
| IDisplayController 21 ClearAppletTransitionBuffer | no transition buffer is modelled |
| IApplicationFunctions 10 / 12 | one application per runtime |
| IApplicationFunctions 72 RequestToSleep | no sleep model |
| IApplicationFunctions 120 ExecuteProgram | other programs are not built |
| IApplicationFunctions 121 / 122 user channel | the channel is filled only by ExecuteProgram |
| IApplicationFunctions 124, 131 | debug / GPU-abort policies |
| IApplicationFunctions 190 | server maintenance overlay: no server |
| IApplicationFunctions 200 GetLastApplicationExitReason | the ApplicationExitReason enum is unverified |
| IApplicationFunctions 220 | audio output policy: no effect on the host |
| IApplicationFunctions 1000 / 1001 | movie maker / JIT |
| ILibraryAppletAccessor 50 / 80 / 81 / 160 | focus suspension, child applets, indirect layer: not modelled |

### Outside my area
`offline.c`'s nifm IRequest::GetAppletInfo (21) is a stub returning an error. Until it hands out `{applet id 0x0f, mode, argument}`, a title never
launches NetConnect.

## How to integrate
- No order constraints. Conflict hotspots:
  - `runtime/src/services/applet.c`. Other r4 items should not touch it; ipc-reference-audit might add fixture expectations, so keep both sides.
  - Docs: an appended runtime-services section and two env rows.
  - `crates/napple-build/src/pipeline.rs`: two small hunks next to the html ones.
- Build: `make -C gfx lib adapter && make -C frontend && make -C input && make -C audio && make -C runtime`, then `make -C runtime test test-services`,
  then `cargo test --release -p napple-build`.
- **Existing title data needs a rebuild to get `legal/`** (the shared `NappleWork/games` was built before this). Without it a title's legal page is
  still the error exit. The rebuild belongs to the integrator / napple-latest, under the build lock; I did not overwrite the shared data.

## How to test
1. `make -C runtime test test-services`. Pass: `{"test":"svc_applet_am","checked":67,"failed":0}` and every applet suite at 0 failed.
2. `cargo test --release -p napple-build`: all ok.
3. Legal page (windowed, about 40 s): rebuild LAS into a scratch dir, then
   `NAPPLE_OPEN_WEB_DOC=20:2:0:ipnotices.htdocs/index.html NAPPLE_SHEET_AUTO=confirm:5000 runtime/build/napple-runtime <scratch>/01006BB00C6F0000/ae16f71e002af8cb --seconds 40 --save-root <tmp>`.
   Pass: `web document: shown, exit reason 0`.
4. Screenshot: `NAPPLE_DEBUG_SAVE_SCREENSHOT=12:/tmp/a.jpg … --headless --seconds 20` on LAS. Pass: `saved to the album (… JPEG)`, and `/tmp/a.jpg`
   shows the game's frame.
5. MK8D Online Play once nifm's GetAppletInfo is real: `CreateLibraryApplet id 0xf` in the log, then the game stays on the menu offline.

## Fix policy hints
- Safe: Linux stub additions for `test_svc_applet_am.c`, include order, doc wording.
- Needs a decision:
  - NetConnect output layout, if a title reads it differently.
  - Whether the exit grace should be shorter in automated runs (env var exists).

## Needs from other areas
- `runtime/src/services/offline.c` (nifm, ipc-reference-audit): IRequest::GetAppletInfo should return NetConnect's id (0x0f), mode and argument
  storage, so titles launch the applet this branch answers.
- napple-latest / integration: rebuild the shared title data so `legal/` exists for every title.

## Integration result
- Date 2026-11-02, merged into main as f73b8ec (branch head cb539e3), after core-ordering-kernel and ipc-reference-audit. One mechanical conflict: `docs/architecture/runtime-services.md`, both appended sections kept (ipc-reference-audit's, then this one). Outcome: **Small fix**.
- `integrate:` fixes (both name-only, closing ipc-reference-audit's two leftover `test_ipc_reference -v` notes): c720417 IApplicationFunctions 210 row named `(unnamed)` as in switchbrew and the fixture (was `GetSystemEvent210`); 47baa0a the am storage object's interface string is `IStorage(am)` (was `IStorage`, compared with fsp-srv's fixture: "row 0 is named Open, the reference says Read"). Nothing compares that string; it only shows in logs and in run summaries (`IStorage(am):0`). After them `test_ipc_reference runtime -v | grep "reference says"`: nothing; fixtures 111, missing 0.
- No native ports: `applet_capture_host.c` already uses the macOS gfx readback; its `#else` branch is a no-op, left as is.
- Build: gfx lib adapter, frontend, input, audio, runtime: rc 0. `make -C runtime GFX=napple test test-services test-ipc-reference`: rc 0, every suite 0 failed (applet 116, applet_ae 52, applet_album 63, applet_am 67, applet_host 539, applet_libs 263, applet_web 219, nifm_audit 122). `cargo test --release -p napple-build`: all ok (lib 53 passed incl. `write_legal_goes_under_legal`, e2e ok). Note: `cargo` is not on the default PATH here; use `PATH=/opt/homebrew/opt/rustup/bin:$PATH` as napple_latest.sh does.
- SaveCurrentScreenshot: LAS headless 20 s with `NAPPLE_DEBUG_SAVE_SCREENSHOT=12:<file>`: `saved to the album (113675-byte JPEG)`, the JPEG is the storm frame on screen. **verified**.
- Four-title boots (60 s, ACNH 90 s, shared data, private save roots, run lock): all rc 0, missing_ipc [], unsupported_svcs [], stubbed_ipc only `nvdrv:13`.
- **Contract with ipc-reference-audit (MK8D Online Play -> NetConnect): not reached on main.** Scripted headless MK8D update run (`NAPPLE_INPUT_SCRIPT=15:A,19:DOWN,20.5:DOWN,22:A`, `NAPPLE_TRACE_IPC=all`): the SDK calls `IRequest` Submit, GetResult (2110-1111), **Cancel**, and only then (1.7 s later) GetAppletInfo. `nifm_request_cancel` (offline_core.c) resets the request's result, so GetAppletInfo answers 2110-1111 and the title never creates the applet; it shows "Connecting..." and goes back to the main menu (no crash). `test_svc_nifm_audit.c:47` pins "Cancel clears the result", so changing it is a design decision, not an integration fix.
  - Experiment (not landed): a rejected request keeps its result after Cancel (only an accepted one is reset). Then MK8D calls GetAppletInfo, `CreateLibraryApplet id 0xf mode 0`, Start, GetResult, PopOutData, re-Submits, and repeats that 20 times (24.8 s - 30.3 s) before giving up (`DeclareCloseOnlinePlaySession`) and showing "A communication error has occurred." with OK. So the console's network-error result is reachable, but the NetConnect output (`{u32 result; u32 pad}`, [unverified]) makes the SDK retry; the real applet's output layout / result should end the loop at once.
- Acceptance 2 (legal page in the web sheet), on the shared data rebuilt from final main 2d3c68c (`napple build --data-only`, all four titles; every build dir now has `legal/`): LAS windowed 40 s, `NAPPLE_OPEN_WEB_DOC=20:2:0:ipnotices.htdocs/index.html NAPPLE_SHEET_AUTO=confirm:5000 NAPPLE_SHEET_SHOT=<dir>`: `web document: shown, exit reason 0, last page file:///.../legal/ipnotices.htdocs/index.html`; the sheet screenshot is LAS's ©1993-2019 Nintendo / GREZZO notice page. **verified**. Correction to "How to test" step 3: it needs a window; with `--headless` the sheet is not shown and the line reads `exit reason 1`.
