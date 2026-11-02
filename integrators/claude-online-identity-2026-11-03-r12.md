---
branch: claude/online-identity-2026-11-03-r12
orchestration: online-identity
base: ac6692cb300be6fc6e90eb894de0268c3732e948
head: HEADPLACEHOLDER
env: mac
date: 2026-11-03
areas: [crates/napple-online]
status: ready-with-caveats
---

# claude/online-identity-2026-11-03-r12: napple-online accepts account JWTs, ghost alias, match log, social reporting

## Summary
napple-online verifies napple-account access JWTs (Ed25519, JWKS cache) when `NAPPLE_ACCOUNT_URL` is set; the NEX pid derives from `profile_id` (mapping in `identity.rs` and `docs/guides/napple-online.md`). Tier-5 players get a random per-session bot-style alias (unique among live players, never a live real nickname). A match log records real account/profile/title/participants/start/end (30-day retention, hourly purge, `POST /online/reports` alias -> real account for admins, export). After a match, encounters and per-title stats go to napple-social with a service credential (ghosts excluded). Without `NAPPLE_ACCOUNT_URL` the legacy self-issued tokens and MK8D wire behaviour are unchanged (all pre-existing tests pass).
NOT done: not run against the real napple-account/napple-social processes or a real MK8D login (needs the other r12 items; the client must send the JWT as the NEX token); legacy `/napple/v1/friends` has no JWT equivalent yet (friends for the MK8D friend-join come from social; mk8d-online-race / client-core decide how).

## Files
New: `crates/napple-online/src/{identity,ghost,matchlog,reporting,hub,upstream}.rs`, `crates/napple-online/tests/hub_server.rs`, change note, this file. Small hooks: `server.rs` (hub field, `with_hub`, `start_login` split out of `login`, hooks in `announce_join`/`leave`/`detach`, Auth 4/5 names), `session.rs` (hub HTTP before legacy API), `main.rs` (env, maintenance thread), `lib.rs` (mods). Shared (append-only): `Cargo.toml` of the crate (+`ed25519-dalek = "2"`; Cargo.lock updated by cargo), `docs/guides/napple-online.md`, `docs/archive/changes/README.md`.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| 1 JWT/JWKS verification, profile-derived pid | tested | `cargo test -j2 -p napple-online` identity::tests (claims, rotation, rate limit, revocation, profile ownership) |
| 2 Ghost alias | tested | ghost::tests, hub::tests, tests/hub_server.rs |
| 3 Match log + purge + report resolution + export | tested | matchlog::tests (retention boundary, restart), hub::tests (admin only) |
| 4 Encounters/stats to social, ghosts excluded, retry | tested | reporting::tests, tests/hub_server.rs |
| 5 Legacy MK8D wire behaviour unchanged | tested | pre-existing 38 server/session tests green (incl. full_login_and_matchmaking_over_prudp) |
| Real services / real game | untested | not run |

## Unbuilt / untested list
Nothing unbuilt (all compiled on macOS). Untested: against real napple-account/social/MK8D.

## Findings
- Decisions: hub only when `NAPPLE_ACCOUNT_URL` is set (no guessed default port, keeps LAN/dev mode); upstream client is a minimal http:// std client (no TLS; services listen on localhost behind Caddy); admin = account UUIDs in `NAPPLE_ONLINE_ADMIN_ACCOUNTS` (the JWT carries no email); NEX login name carries `profile/title` since the JWT has neither title nor profile; encounters list non-ghost participants only and are the shared-lobby proof.
- social API assumptions to confirm: service credential `Authorization: Bearer $NAPPLE_SOCIAL_SERVICE_TOKEN` + `X-Napple-Service: napple-online`; `POST /social/v1/stats` accepts `profile_id` when called with it; `/encounters` accepts the service credential. Tier comes from the `tier` field of `GET /account/v1/profiles` (defaults to 1 if absent).
- The legacy pid for the same player differs from the new one (profile-derived); saves/friends keyed on the old pid will not carry over.

## How to integrate
- No ordering constraint. Conflict hotspot: `server.rs` (mk8d-online-race edits it): my edits are ~6 small hunks (hub field/with_hub, login split, hooks in announce_join/leave/detach, two Auth name lines).
- `cargo test -j2 -p napple-online` -> 49 + 1 pass.
- Run: `NAPPLE_ACCOUNT_URL=http://127.0.0.1:PORT NAPPLE_SOCIAL_URL=http://127.0.0.1:PORT2 NAPPLE_SOCIAL_SERVICE_TOKEN=... NAPPLE_ONLINE_ADMIN_ACCOUNTS=<uuid> napple-online serve`.
- No Linux-only calls.

## How to test
1. `cargo test -j2 -p napple-online` (pass = 0 failed).
2. Real stack (online-tieup): client sends the account access JWT as the NEX token with login name `<profile_uuid>/<title_id>`; check `<data>/matchlog/matchlog.jsonl` after a match and social's recent players.
3. Regression: unset `NAPPLE_ACCOUNT_URL`, legacy `/napple/v1/token` path works.

## Fix policy hints
Safe to fix: env/URL typos. Design decision: if social's service-credential shape differs, change only `reporting.rs` request building.

## Needs from other areas
- napple-social: accept the service credential on `/encounters` and `/stats` (with `profile_id`), as above.
- client core (runtime): send the account JWT as the NEX token and `<profile_uuid>/<title_id>` as the login name.
- mk8d-online-race: friend list for friend-join no longer arrives via `/napple/v1/friends` in account mode.
