---
branch: claude/client-core-2026-11-03-r12
orchestration: client-core
base: ac6692cb300be6fc6e90eb894de0268c3732e948
head: c2089e94f3f09b98e7c8a92919074034b0a81d17
env: mac
date: 2026-11-03
areas: [crates/napple-cloud, runtime/src/online, runtime/src/net]
status: integrated
---

# claude/client-core-2026-11-03-r12: Napple Online client core

## Summary
Shared credentials (0600 file, AES-256-GCM under a Keychain key `lat.napple.online`/`wrap-key`, file-key fallback, flock + atomic rename), profile map,
auth/profile/social client (Rust, ureq), the overlay function table in C over it, and strong `napple_account_access_token` / `napple_account_profile_id`
that switch ONLINE_CAP_FRIENDS on for a signed-in Mac (the r10 friend service does the rest). NOT done: WebSocket stream (polling every 30 s + after each
action), overlay-side 60 s heartbeat (net_social heartbeats in game), recovery-phrase creation for a first device (the key is made and granted, no recovery record posted),
runtime main.c wiring (see Needs), iOS.

## Files
New: crates/napple-cloud/src/online/{mod,creds,client,ffi,tests}.rs, runtime/src/online/online_ui.{c,h}, runtime/tests/test_online_ui.c, docs/guides/napple-online-client.md, change note.
Edited: crates/napple-cloud/src/{lib.rs (+1 line), store.rs (Keychain::with_service)}, runtime/Makefile (append), runtime/src/net/net_online.h + net_online_social.c (profile hook), docs/archive/changes/README.md (append), frontend/include/napple_online_ui.h (copied from overlay-account's branch commit 42f7563; identical, will merge clean).

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Credentials: 0600, wrapped, concurrent-safe | tested | cargo test -p napple-cloud online (7 pass) |
| Auth/profile/social client vs fake server | tested | same; fake server is my reading of the contract/guide, NOT the real account/social |
| Overlay table + callbacks, fake server in C | tested | make -C runtime test-online: 27 checks, 0 failed |
| Friend service hooks (token, profile) | tested (hook level) | test_online_ui; net_social_config consumes them (not exercised against a real napple-social) |
| make -C runtime GFX=napple test test-services test-ipc-reference | verified | exit 0 (needed gfx/frontend/input/audio libs built first in the worktree) |
| MK8D + SMO headless 20 s, NAPPLE_NET unset and =host | verified | rc 0, ~same log length, no online dir created when signed out. LAS/ACNH not run |

## Unbuilt / untested list
Real end-to-end against napple-account/napple-social r12 servers (online-tieup). Keychain path of the wrap key was not exercised in tests (tests use the file key); cargo build of the real keychain path compiles on macOS.

## Findings
- Endpoint guesses to check against the final servers: photo multipart field `photo`; PATCH /profiles/{id} {tier}; GET /search?q=; GET /recent; GET /profiles/{id}/stats shapes (`stats[]` or `titles{}`); POST /reports; POST /friend-requests {profile_id} when a UUID is passed; token answer shape (flat or under `tokens`; account/device ids fall back to JWT sub/did).
- Cloud saves still use their own Session (cloud/<ns>, separate login). "Save follows" needs napple-cloud's Session to read these shared credentials; not changed (cloud/ paths are outside this item). The account save key is already stored here (account_key_enc) for that.
- The runtime unit tests link without CLOUD, so the strong hooks only exist in napple-runtime/applet dylib (same as cloud.c).

## How to integrate
- Merge after overlay-account (header is already identical). Hotspots: runtime/Makefile (append at end), napple-cloud lib.rs.
- Build: export PATH to cargo; make -C runtime GFX=napple build/napple-runtime; make -C runtime test-online.

## How to test
1. cargo test -j2 -p napple-cloud online. 2. make -C runtime test-online. 3. make -C runtime GFX=napple test test-services test-ipc-reference. 4. Boot a title headless with NAPPLE_HOME=<empty dir>: no <dir>/online created.

## Fix policy hints
Endpoint/shape mismatches are small edits in client.rs (act / build_state).

## Needs from other areas
- runtime main / frontend host (not mine): call napple_online_runtime_start(NULL,NULL,NULL,<active uid hex>) after the frontend exists, pass the result to napple_frontend_set_online_ui, and call napple_online_ui_pump() once per frame on the main thread (declared in runtime/src/online/online_ui.h).
- cloud saves: let crates/napple-cloud Session use <NAPPLE_HOME>/online/credentials.json.

## Integration result
- Date: 2026-11-03 (batch r12, rolling). Merged at e3b69e7. Outcome: Small fix + mechanical conflict (changes index).
- integrate: fixes: docs/guides/README.md links napple-online-client.md (check_docs orphan). Not done by the author and still open at close: runtime main does not yet call napple_online_runtime_start (see the follow-up wiring commit in this batch)
- Tests: cargo test -p napple-cloud 25 passed; make -C runtime GFX=napple test test-services test-ipc-reference test-online: 0 failed (test-online 27 checks); make -C frontend test settings 77/0 window 13/0; four-title boots plain and NAPPLE_NET=host: rc 0, missing_ipc [], unsupported_svcs [], notify-running, no online dir created; check_docs only archive orphans; gen_support OK
- Corrections to instructions: make -C gfx adapter is needed before make -C runtime GFX=napple in a fresh worktree; cargo needs PATH=/opt/homebrew/opt/rustup/bin
