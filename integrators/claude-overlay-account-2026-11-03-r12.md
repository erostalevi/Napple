---
branch: claude/overlay-account-2026-11-03-r12
orchestration: overlay-account
base: ac6692c
head: HEADPLACEHOLDER
env: mac
date: 2026-11-03
areas: [frontend]
status: ready-with-caveats
---

# overlay-account: Online tab in the overlay (shell, account/profile screens, UI API header)

## Summary
`frontend/include/napple_online_ui.h` defines the function table (state getters, async actions with completion `(request_id, result, json)`, change callback), `napple_frontend_set_online_ui()` to install it, `napple_online_ui_stub()` and `napple_online_register_subtab()` for overlay-social. The overlay has a fifth **Online** tab: sign in / create account (13+ and terms checkboxes with links) / 6-digit code / reset password; signed in: header, profile editor (nickname, bio, 3 links, 48 icons, NSOpenPanel photo + preview), tier picker with one-line descriptions, profiles (<=8), devices approve/revoke, cloud-save status, account deletion with confirm. Controller: text fields are buttons that open the software keyboard bar, so the existing focus ring/A drives everything. Stub with canned data makes all screens work without servers.
Not done: friends/stats/invites screens (overlay-social); real-pad and real-keyboard-bar runs; Friends sub-tab strip only exercised with a fake sub-tab.

## Files
New: `frontend/include/napple_online_ui.h`, `frontend/src/overlay_online.{m,h}`, `overlay_online_stub.m`, `frontend/tests/test_online_ui.m`, `frontend/tests/online_ui_shots/*.png`, change note. Changed: `frontend/src/overlay.m` (5th tab: makers array, labels, nav_tab modulo 5, select_tab bound, include), `frontend/Makefile` (test-online-ui target; also in `make test`), `docs/architecture/audio-input-frontend.md` (appended). `settings.c` not touched.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Header compiles, `make -C frontend` | verified | -Wall -Wextra -Werror clean |
| Screens vs stub (sign-in error, signup validation, verify, tier, profile save, profiles, devices, saves, delete, reset, unverified login, sub-tab) | verified | `make -C frontend test-online-ui`: 38 checks, 0 failed |
| Per-screen PNGs | verified | 15 PNGs in `$TMPDIR/napple-online-ui` (6 kept in tests/online_ui_shots) |
| Existing tabs/tests | verified | `make -C frontend test`: settings 77/0, window 13/0 |
| Tab inside a live game, controller nav, real text bar | untested | |

## Unbuilt / untested list
Nothing unbuilt. Untested: pad navigation on the Online tab, `fe_textbar_show` path from the field buttons (tests set values directly; `NAPPLE_SWKBD_ANSWER` is honoured by the field buttons), the five-segment tab strip width on a 400 pt panel (visually unchecked).

## Findings
- Decisions: terms/privacy links point to `https://napple.lat/terms` and `/privacy` (legal item may change them in `legal()`); the API `ctx` is passed as first arg; lists use caller-storage getters returning total count; `napple_online_register_subtab` page makers return a +1 retained NSView (`__bridge_retained`); the `search` completion json shape is documented in the header; tier names Public / Friends (searchable) / Friends (hidden) / Private / Ghost.
- The B button on the controller still closes the overlay (existing behaviour); screens have explicit Back buttons.
- Sign-in state drives the screen; `set_change_callback` is claimed by the tab while it is shown (one callback at a time).

## How to integrate
- client-core: build the table, call `napple_frontend_set_online_ui(fe, &api)` before the overlay opens (the page asks `API->get_account` on each rebuild). overlay-social: register sub-tabs in a constructor or at startup with `napple_online_register_subtab`.
- Conflict hotspot: `frontend/src/overlay.m` (tab arrays) and `frontend/Makefile`.
- Build: `make -C frontend && make -C frontend test`.

## How to test
1. `make -C frontend test-online-ui` -> "38 checks, 0 failed", PNGs in `$TMPDIR/napple-online-ui` (or `NAPPLE_ONLINE_SHOT_DIR`).
2. `make -C frontend test` regression.
3. Human: open a game, F1, Online tab; with `NAPPLE_ONLINE_STUB_SIGNED_IN=1` it starts signed in; stub code `123456`.

## Fix policy hints
Layout/cosmetic issues are safe to fix. The API shape is a contract: changing it needs client-core and overlay-social.

## Needs from other areas
client-core: implement the table; legal: confirm the terms/privacy URLs; overlay-social: use the registration API.
