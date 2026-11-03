# napple-social (2026-11-02, batch r9): Napple friends and presence service

Branch `napple-social-cloud-2026-11-02-r9` (Linux cloud session, base `main` = `57431a7`). Status: *tested* (Linux, `cargo test -p napple-social`,
40 tests); not deployed, not yet run against the real napple-account.

## What
A new standalone crate and binary, `crates/napple-social`: Napple's own friends and presence service for Napple players (never Nintendo's), meant to run
next to napple-account, napple-online and napple-room behind `pretend.napple.lat`. REST JSON under `/social/v1` and a WebSocket at `/social/v1/stream`.
API and data model: [napple-social guide](../../guides/napple-social.md).

- Auth: napple-account Ed25519 JWT access tokens verified through its JWKS (cached 10 min, re-fetched on an unknown `kid` at most every 10 s, revoked
  keys refused after a refresh, known keys kept through an outage). Claims `sub`/`did`/`aud`="napple"/`exp`, 30 s leeway.
- Profile ownership: `X-Napple-Profile` checked against `GET /account/v1/profiles` with the caller's token, cached 60 s per account.
- Friend codes `NP-dddd-dddd-dddd` (11 random digits + Luhn), requests (send by code, accept, decline, cancel, crossed requests auto-accept), remove,
  block/unblock, 300-friend limit, pending and block limits.
- Presence online / playing {title_id, title_name, mode, join_token} / offline, heartbeat every 60 s, offline after 3 missed heartbeats (sweeper),
  last seen; privacy `presence_visibility` friends/nobody, `appear_offline`, `accept_friend_requests`.
- Invites to a joinable session, delivered over the WebSocket (and replayed on connect), 10-minute expiry.
- SQLite WAL in `NAPPLE_SOCIAL_DATA_DIR`, env-only config, in-memory token-bucket rate limits (IP, account, friend requests, invites, code regeneration),
  stdout logs with a per-request access line, graceful SIGTERM.

## Decisions
- Friend codes use the prefix `NP` (Napple), not `SW`; the check digit rejects typos before a lookup.
- Presence is explicit (`PUT /presence`); opening the WebSocket does not mark a profile online.
- A profile that blocked you is indistinguishable from a wrong code (404 `code_not_found`).
- `appear_offline` freezes the friends-visible last seen; `nobody` hides it entirely.
- Requests and invites are deleted when answered (no history kept).
- `DELETE /social/v1/me` erases a profile's social data; napple-account should call it when it deletes a profile.
- The first unknown-`kid` refresh is throttled relative to the last JWKS fetch (startup included).
- Single process, single SQLite connection: live events are in-process, so one replica per database.

## Files
`Cargo.toml` (workspace member), `Cargo.lock` (new packages only; no existing version changed), `crates/napple-social/**`, this note,
`docs/guides/napple-social.md`, the hand-off notes.
