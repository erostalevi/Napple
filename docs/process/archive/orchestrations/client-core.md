---
name: client-core
batch: 2026-11-03-r12
branch: claude/client-core-2026-11-03-r12
base: orchestrate/2026-11-03-r12
env: mac
model: claude-opus-5-5
depends_on: []
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_942fcff3-c0f6-4332-83d4-d6c57f50976c
launched_at: 2026-11-03
---

# client-core: runtime client core: shared credentials, auth/profile/social client, local-user mapping, presence, provider hook wiring, overlay C API implementation

Read `orchestrations/ONLINE-CONTRACT.md` and `orchestrations/ENV-MAC.md` first; the contract is the source of truth for APIs, env names, tiers and the C API.

## Owned paths
new `runtime/src/online/` (client core + implementation of `frontend/include/napple_online_ui.h`'s function table), `runtime/src/net/net_online*` (wire the friend and cloud OnlineProvider hooks added in round 11), `crates/napple-cloud/` (shared client code), `runtime/Makefile` (append), tests `runtime/tests/test_online_*.c`, `docs/guides/napple-online-client.md`, change note. The header `frontend/include/napple_online_ui.h` is owned by overlay-account: coordinate by reading the contract; if it is not there yet, write against the contract's list and rebase onto it.

## Do not touch
Everything else: other r12 items edit the other paths in parallel (account auth/profiles, social, online server, client core, overlay, MK8D protocol, infra, legal), `orchestrations/`. Shared files (`Cargo.toml` members, `Cargo.lock`, `runtime/Makefile`, `docs/index.md`, `docs/archive/changes/README.md`): append-only and minimal. Use `cargo build -j2` / `cargo test -j2` (several sessions build at once on a 16 GB Mac).

## Work and acceptance
1. Credentials file + Keychain-wrapped key exactly as in the contract (0600, shared across all Napple games on the Mac, concurrent-safe: two games refreshing the token at once must not corrupt it: file lock + atomic rename).
2. Auth/profile/social client (Rust in napple-cloud or C with libcurl-free HTTP as the runtime already does for cloud saves; reuse what exists): signup/verify/login/reset/logout, token refresh, profile CRUD + photo upload, friends/requests/blocks/search/recent/stats/report, device approval, presence heartbeat (60 s) + join tokens; WebSocket stream for presence/invites if napple-social exposes it.
3. Local Switch user -> Napple profile mapping (automatic; no linking step) and passing tier/ghost state to the online glue.
4. Implement the overlay function table (state query + async actions + change callback) over the client; unit-test with a fake HTTP server.
5. Wire ONLINE_CAP_FRIENDS and the cloud/account hooks (friend images/requests, GetFriendList with presence, acc tokens, olsc) in `net_online*` so MK8D's friend service shows Napple friends and presence; test with fakes.
6. `make -C runtime GFX=napple test test-services test-ipc-reference` green; four-title boots unchanged with and without NAPPLE_NET=host.

## Env notes
Shared title data read-only; game runs under `lockf -k /Users/neto/NappleWork/.run.lock` (<=120 s, private --save-root); local only, never push; never ask the user; never commit credentials or Nintendo material. Test-first, small commits.

## Result
Outcome: Small fix + mechanical conflict (changes index). Merged at e3b69e7. Details: [integrator result](../integrators/claude-client-core-2026-11-03-r12.md).
