# overlay-account (r12): Online tab in the overlay

New `frontend/include/napple_online_ui.h` (the UI API function table), `frontend/src/overlay_online.m` (tab shell + account/profile screens), `overlay_online_stub.m` (canned-data implementation), `overlay_online.h` (internal), a fifth "Online" segment in `overlay.m` (tab maker, tab count in nav/select), `frontend/tests/test_online_ui.m` and `make test-online-ui`.

Verified on the Mac: `make -C frontend test` (settings 77, window 13, online UI 38 checks, all pass); every screen renders to PNG. Not verified: the tab inside a running game, controller navigation with a real pad, the real software-keyboard bar (the tests set field values directly), and any real server (client-core implements the table).
