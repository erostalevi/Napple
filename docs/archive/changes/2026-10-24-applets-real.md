# applets-real (2026-10-24, plan11)
- swkbd: config parsed in `applet_swkbd_parse` (type, min/max, password; max clamped to 500); numpad type 1 now reaches `frontend_text_input(numeric)`; `applet_swkbd_fit` cuts text to max code points and treats text under min as cancel (applies to NAPPLE_SWKBD_ANSWER too).
- controller: `applet_controller_answer` takes the connected-pad count (`frontend_connected_pads()`: input snapshot, `NAPPLE_CONTROLLER_PADS=n` override, 1 headless); players = connected clamped to min..max; `short_of_min` logged.
- error: reports a code text ("CCCC-NNNN" / "2MMM-DDDD", layouts unverified against a title) and shows it through `frontend_alert` (NSAlert on the main thread in window mode, log line headless).
- player select: `applet_player_select_pick` (index / cancel); `NAPPLE_PLAYER_SELECT=cancel|<index>`. Only one profile exists (account.c belongs to ac-games).
Verified: `make test`, `make test-services` (applet_libs 97 checks, 0 failed). Not run: LAS/SMO 45 s headless boots, window-mode NSAlert/pad count (no game data/GUI in this worktree).
Files: runtime/src/{app.m,frontend.h}, runtime/src/services/{applet.c,applet_lib_core.[ch]}, runtime/tests/{stubs.c,test_svc_applet.c,test_svc_applet_libs.c}.
