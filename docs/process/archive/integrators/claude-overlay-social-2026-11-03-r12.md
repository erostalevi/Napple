---
branch: claude/overlay-social-2026-11-03-r12
orchestration: overlay-social
base: 085785db1056dd6c04dc5ed76bc407c953bb9527
head: ed7e776
env: mac
date: 2026-11-03
areas: [frontend/overlay]
status: integrated
---

# overlay-social: friends, requests, add/search, recent, blocked, stats sub-tabs + toasts

## Summary
Six sub-tabs registered through `napple_online_register_subtab` (header from overlay-account, commit fffcb0b cherry-picked from 42f7563), built only on `napple_online_ui_api`. Rows are plain NSButtons so FENav controller navigation reaches them; text goes through a host on-screen-keyboard callback (`Type...`) or the hardware keyboard. Toasts for new incoming friend requests (diffed in `napple_online_social_changed`) and invites (`napple_online_social_invite_event`). NOT done: overlay-account's shell (`overlay_online.m`, `napple_online_ui_stub`) was not on the base/branch (only the header), so the pages were tested against my own canned API and a faked register function, and are not yet visible in a real overlay. Join-token handling, invite-received source and a real controller-driven run are untested.

## Files
frontend/include/napple_online_social.h (new), frontend/src/overlay_online_social.m (new), frontend/tests/test_online_social.m (new), frontend/Makefile (appended target test-online-social), docs/archive/changes note, frontend/include/napple_online_ui.h (cherry-pick from overlay-account).

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Library build -Werror | verified | make -C frontend libnapple_frontend.a |
| Social pages + actions + toasts | tested | make -C frontend test-online-social: 28 checks, 0 failed; PNGs of all six pages written to $NAPPLE_SOCIAL_SHOT or $TMPDIR (friends/add inspected visually) |
| Existing frontend tests | verified | make -C frontend test: settings 77/0, window 13/0 |
| Controller navigation | untested | relies on FENav collecting NSButtons |
| Real shell integration | unbuilt | needs overlay-account's overlay_online.m |

## Unbuilt / untested list
Integration into the real tab shell; on-screen keyboard callback; controller navigation over these pages; real toast display (NappleFE -toast:).

## Findings
- Decision: since the api has no get_invites, "invite received/accepted" toasts come from `napple_online_social_invite_event` that client-core/shell must call; request toasts are derived from get_requests diffs (first call only primes).
- Decision: Recent page has one shared "Report reason" popup; nickname search results come from the search completion JSON.
- Nickname-based friend_request is not sent (api takes code or profile id): nicknames go through Search then Add.
- Page makers return a retained NSScrollView (void*); the shell should take ownership (`__bridge_transfer`).

## How to integrate
- Merge after overlay-account (shares napple_online_ui.h; identical file, cherry-pick will merge cleanly).
- In the shell: call `napple_online_social_init(&host)` at startup (host.toast -> [fe toast:], host.text_input -> napple_frontend_text_input_blocking), forward its change callback to `napple_online_social_changed(api)`, and `napple_online_social_invite_event` from client-core events.
- Makefile: one appended target; no other shared files.

## How to test
1. `make -C frontend test-online-social` : 28 checks, 0 failed, PNGs written.
2. `make -C frontend test`: regression.
3. After the shell lands: open Online tab signed in, check six sub-tabs with the stub, navigate with a controller.

## Fix policy hints
Layout/size tweaks safe. Hooking into the shell is the main integration step.

## Needs from other areas
overlay-account: call the three hooks above (or tell me the extension points); client-core: invite events and presence-based `joinable_token`.

## Integration result
- Date: 2026-11-03 (batch r12, rolling). Merged at 2e795f6. Outcome: Small fixes + mechanical conflicts (frontend/Makefile .PHONY, napple_online_ui.h add/add).
- integrate: fixes: (1) test_online_social uses the real shell sub-tab registry (duplicate napple_online_register_subtab); (2) REQUIRED wiring: overlay_online.m calls napple_online_social_init (host toast -> game view, text input -> keyboard bar via nested run loop; the blocking guest-thread API would deadlock on the main thread), keeps a persistent change callback feeding napple_online_social_changed, social test added to make test, shell test checks Account + six sub-tabs; (3) runtime/src/app.m installs the client-core table (napple_online_runtime_start + napple_frontend_set_online_ui + 0.1 s main-thread pump) with weak defaults for CLOUD=0; verified windowed (Online tab in LAS shows the real signed-out screen even with NAPPLE_ONLINE_STUB_SIGNED_IN=1); (4) guide documents the wiring
- Tests: make -C frontend test: settings 77/0 window 13/0 online-ui 55/0 online-social 28/0; runtime test/test-services/test-ipc-reference/test-online 0 failed; eight boots rc 0 missing_ipc [] except one SMO plain SIGSEGV at sdk+0x1fe608 (2.6 s, taken under concurrent make load; 5 clean reruns); CLOUD=0 runtime links
- Corrections to instructions: invite toasts (napple_online_social_invite_event) have no feed from client-core yet; Needs 'overlay-account shell call the hooks' done by the integrator
