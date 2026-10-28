# applets-on-title (batch 2026-10-27-b7): the library applet sheets over a running title

b6 built the AppKit sheets (controller, error, player select, web, album) but no title had raised them. This batch puts them in front of a real title's window, adds an e2e step, and fixes the focus / input bug that showing one exposed.

## What changed
* **Input and focus (bug fix, `frontend/src/applet_sheets.m`, `frontend.m`)**: while a sheet was up the game kept receiving keyboard / pad input (the text bar already blocked it, the sheets did not), and the game view did not get the keyboard back afterwards. `fe_sheet_begin` / `fe_sheet_end` (frontend.m) count the open sheets, block the game's input through `refreshOverlayInputMode`, and on the last one make the window key and give the game view first responder again. Alert sheets and window sheets (web, album) both use them.
* **`NAPPLE_DEBUG_SHEETS=<seconds>`** (`runtime/src/app.m`, window mode only): raises every sheet (error, controller, player select, software keyboard, web on a generated `file://` page, album) from a thread of its own, the way a guest thread would, through the same `ui_*` hooks the applets call, over the running title; logs `debug-sheets: <sheet> -> <result>`. `NAPPLE_CONTROLLER_PADS=n` sets the connected pads the controller sheet shows.
* **Text bar test aid** (`textbar.m`): with `NAPPLE_SHEET_AUTO` set (and no `NAPPLE_SWKBD_ANSWER`) the inline keyboard bar is screenshotted (`swkbd.png`) and answered (cancel -> Cancel, else OK), so the inline header / sub strings are visible in the shot and the real bar runs without a human. `auto_action` / `shot_window` are shared from applet_sheets.m.
* **e2e step `applets`** (`tools/e2e/e2e.py`, first title, `--no-applets` skips): windowed run 1 answers every sheet "confirm" (asserts the six result lines and a PNG per sheet), run 2 answers "cancel" with `NAPPLE_SWKBD_ANSWER=Epona` (controller 0 players, player select -1, scripted swkbd text). Audio off (`NAPPLE_AUDIO_DEVICE=0`).

## Matrix: which applet each title raises, verified vs unit-tested
| Applet | Reference titles at boot | Live over a title (LAS window) | Unit tested |
|---|---|---|---|
| swkbd (inline bar) | LAS / SMO / ACNH / MK8D: not in the first minutes of boot; the Mii Maker firmware applet (e2e `mii-db`) raises it | sheet + bar via NAPPLE_DEBUG_SHEETS, answered | test_window (answers), test_svc_applet_swkbd |
| controller support | none seen at boot (MK8D/SMO raise it only from menus) | sheet via NAPPLE_DEBUG_SHEETS | test_svc_applet_libs, test_sheets |
| error | none (needs a guest error path) | sheet via NAPPLE_DEBUG_SHEETS | test_svc_applet_libs, test_sheets |
| player select | none | sheet via NAPPLE_DEBUG_SHEETS | test_svc_applet_libs/host, test_sheets |
| web / offline web | none | sheet (offline page) via NAPPLE_DEBUG_SHEETS | test_svc_applet_web, test_sheets |
| album | none | sheet (select) via NAPPLE_DEBUG_SHEETS | test_svc_applet_album, test_sheets |
"Live" means the sheet UI ran over the real title's window through the applet UI hooks; it is not the guest's own applet call (no reference title raised one in the boot window, and `runtime/src/services/applet*.c` belongs to applets-live). The guest-side scripted answers (`NAPPLE_PLAYER_SELECT`, `NAPPLE_WEB_ANSWER`, `NAPPLE_ALBUM_PICK`, `NAPPLE_CONTROLLER_SUPPORT`) are checked inside the applets before the hooks and stay covered by the runtime unit tests; an e2e step for them needs a title (or a debug launch in applet.c) that raises the applet: N/A here.

## Not done / follow-ups
* A debug launch of the non-Mii library applets from applet.c (like `NAPPLE_DEBUG_LAUNCH_APPLET`) so the scripted guest-side answers can be e2e-tested: applets-live owns applet.c.
* Gamepad navigation of the sheets; fullscreen game window interplay.
* Multi-pad controller assignment beyond the count shown by the sheet (`NAPPLE_CONTROLLER_PADS`): the sheet reflects the count only.

## Update: verification
`run.sh las --only applets` PASS (confirm + cancel runs), the other `run.sh las --no-golden` steps PASS (run split with new `--build-only` / `--only` flags so each game-lock turn stays short). Screenshots of the six sheets over the LAS window: `docs/screenshots/applets-on-title/`. New aids: `NAPPLE_SHEET_ORIGIN`, `NAPPLE_SHEET_SIZE` (place the sheet window before the shot).
