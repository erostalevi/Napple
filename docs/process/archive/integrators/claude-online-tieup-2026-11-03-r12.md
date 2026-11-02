---
branch: claude/online-tieup-2026-11-03-r12
orchestration: online-tieup
base: 331dd05fc8438c428896cdab25d51d0cb8af913c
head: f886c7b45934471d322912d12a0600e56281b16e
env: mac
date: 2026-11-03
areas: [crates/napple-cloud, runtime/src/net, tools/e2e/online, infra/local]
status: integrated
---

# claude/online-tieup-2026-11-03-r12: Napple Online end to end

## Summary
Scripted definition of done run against the local stack (dev mailer) and LIVE on https://pretend.napple.lat: 60/60 checks each. Live used real SES mail
(tieup-{a,b,c}<tag>@napple.lat, forwarded to Gmail), a real S3 cloud save and cleaned up (all three accounts deleted). Overlay Online tab driven by the real
client (sign-in through its buttons, 12 PNGs). Six integration bugs fixed. NOT verified: an MK8D race / Wireless Play / Friends list in game (see below).

## Files
tools/e2e/online/{online_e2e.py,overlay_shot.m,build_overlay_shot.sh}, crates/napple-cloud/examples/online_cli.rs, docs/guides/online-e2e.md, change note;
fixes in crates/napple-cloud/src/{session.rs,ffi.rs,online/client.rs}, runtime/src/net/net_online{.h,_http.c}, infra/local/run.sh (1 line), runtime/tests/test_net_online_identity.c.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| signup/verify/login(unverified 403)/second device/E2E key approval | verified local+live | online_e2e.py 60/60 |
| profile edit, https-only links, photo (PNG ok, junk 422), tier | verified local+live | same |
| cloud save A -> B before mount (live S3 presign) | verified live | same ("device B pulled the save before mount") |
| friends by code A<->B; tier matrix (search/request/presence) | verified local+live | same |
| logout, delete_account, login after delete refused | verified local+live | same |
| overlay Online tab with the real client (signs in via buttons, friends/profile pages) | verified (local stack) | PNGs in notes folder online-tieup-overlay/ |
| MK8D headless boot signed in: friend service on, presence "playing" visible to a friend, linked as profile principal id | verified | run log; friend's state showed online MK8D title |
| make test test-services test-ipc-reference; cargo test -p napple-cloud (25) | see change note / below | |
| MK8D race, Friends list in game, Wireless Play | NOT verified | needs a human at the menu; mk8d-online-race owns it. Headless 90 s stays in boot; no NEX login happens |

## Unbuilt / untested list
Keychain wrap-key path (tests use the file key), push/pull with the real runtime binary (the CLI calls the same FFI).

## Findings (fixed, one commit each)
1. Cloud saves could not sign in: napple-cloud Session still used /auth/email/* (removed). Session::open now uses the shared online credentials (Session::open_shared) and links the local user via profile_map.
2. Save key divergence risk: grants carried no key_id, so a second device saw key_state=bootstrap and made a different key. All grants now send key_id; bootstrap only when no other device exists.
3. Tier change not applied by napple-social until its 10 s snapshot TTL (contract says immediately): client pokes social after set_tier. Others still see the old tier for <= NAPPLE_SOCIAL_PROFILE_TTL_SECS (10 s). Real fix would be account->social push; not done.
4. DELETE /me needs {confirm_email}; client sent a password.
5. infra/local/run.sh set NAPPLE_ACCOUNT_PUBLIC_BASE without /account/v1: dev-blob upload URLs 404.
6. Game side still used /napple/v1/account (410 with account logins on): signed-in Macs now use principal_id(profile) and the access token as ID token (net_online_http.c).

## Not fixed / open
- The NEX login name should carry <profile>/<title16> when the user picked a non-first profile; the client sends only the token (server then uses the account's first profile).
- Overlay: friend avatar draws the icon number ("0") instead of the glyph for icon 0 (overlay-social); photo for friends is not fetched (only the current profile's photo is cached).
- No WebSocket stream client; state polls every 30 s and after actions; invites arrive on the next poll.
- SES sandbox: only @napple.lat / the two verified Gmail addresses can receive codes; a failed send still leaves an unverified account holding the nickname (signup answers mail_failed).
- sign-up code mails are limited per IP (20/h): restart the local stack between e2e runs.

## How to integrate
Merge after main fdcb30a+. Hotspots: runtime/src/net/net_online_http.c, crates/napple-cloud/src/session.rs. Build: cargo build -p napple-cloud --example online_cli; make -C runtime GFX=napple build/napple-runtime.

## How to test
1. infra/local/run.sh start (NAPPLE_SOCIAL_PROFILE_TTL_SECS=2) then python3 tools/e2e/online/online_e2e.py : 60/60. 2. cargo test -p napple-cloud; make -C runtime GFX=napple test-net-online test-online. 3. Live: docs/guides/online-e2e.md.

## Fix policy hints
Endpoint-shape issues are small edits in crates/napple-cloud/src/online/client.rs.

## Needs from other areas
mk8d-online-race: login name with profile; overlay-social: icon-0 avatar; napple-social: apply tier changes at once (or lower the TTL).

## Integration result
Date 2026-11-03 (rolling r12). Merged as 656d82c. Outcome: Clean (no code fixes); one docs fix: `integrate: link the online-e2e guide from the guides index` (check_docs orphan).
No native ports; nothing to port. Tests: cargo build/test -j2 --workspace --locked all green (napple-cloud 25); make -C runtime GFX=napple test test-services test-ipc-reference test-net-online test-online 0 failed;
make -C frontend test 0 failed; local stack (NAPPLE_SOCIAL_PROFILE_TTL_SECS=2) + online_e2e.py 60/60, stack stopped; four-title boots (SMO, MK8D update, LAS, ACNH update) plain and NAPPLE_NET=host under the 3-slot game lock,
private save root and empty NAPPLE_HOME: rc 0, missing_ipc [], unsupported_svcs [], last_stage notify-running, no online dir created; check_docs only archive orphans; gen_support OK.
Corrections: the scratch worktree needs `make -C gfx adapter` (and audio/input/frontend built) before runtime tests link; the data dir for boots is games/<id>/<build dir> (the one holding manifest.json).
Live run on pretend.napple.lat and the in-game MK8D race were not re-run.
