---
branch: claude/napple-social-cloud-2026-11-02-r9
orchestration: napple-social-cloud
env: linux-cloud
status: integrated
---

# Integrator note: napple-social (claude-napple-social-cloud-2026-11-02-r9)

Branch `claude/napple-social-cloud-2026-11-02-r9`, from `main` @ `57431a7`. Linux cloud session. Not merged, not deployed.

## Summary
New standalone crate + binary `crates/napple-social`: the Napple friends and presence service (Napple's own community service for Napple players,
never Nintendo's). REST JSON under `/social/v1` + one WebSocket `/social/v1/stream`. Per-profile social graph; identity from napple-account
(Ed25519 JWT via JWKS + the account's profile list). Features: `NP-` friend codes (12 digits, Luhn), friend requests (send by code, accept, decline,
cancel; crossed requests auto-accept), remove, block/unblock, 300-friend limit, presence (online / playing {title_id, title_name, mode, join_token} /
offline, 60 s heartbeat, offline after 3 missed), last seen, invites over WebSocket (10 min expiry, replayed on connect), privacy
(visibility friends/nobody, appear offline, accept requests), rate limits, SQLite WAL, env-only config, `GET /social/v1/healthz`, stdout logs.
Full API, WebSocket events, error codes and data model: `docs/guides/napple-social.md`.

## Files
- `Cargo.toml`: `"crates/napple-social"` appended to `members` (only change).
- `Cargo.lock`: new packages only (axum 0.8, tokio, rusqlite 0.37 bundled, reqwest 0.12 rustls/ring, ed25519-dalek 2, tokio-tungstenite dev...). Checked: no
  existing package version changed. If other branches also touched the lock, take the other side's lock on conflict and run `cargo build -p napple-social`
  (it only adds entries; no `cargo update` needed), then `cargo test --workspace --locked`.
- `crates/napple-social/Cargo.toml`, `src/{lib,main,api,auth,clock,codes,config,db,error,hub,ratelimit,stream}.rs`,
  `tests/{auth,friends,presence,invites}.rs`, `tests/common/mod.rs`.
- `docs/guides/napple-social.md`, `docs/archive/changes/2026-11-02-napple-social-cloud-2026-11-02-r9.md`, this file.

## Needs a one-line edit outside my scope (the docs CI check fails until done)
`python3 tools/check_docs.py` (run by `tests.yml` "docs" job) reports both new docs as orphans because I was told not to touch other files. Add:
1. `docs/guides/README.md`, in the first table (e.g. above `network-providers.md`):
   `| [napple-social.md](napple-social.md) | The Napple friends and presence service (`crates/napple-social`): API, WebSocket events, privacy, data model, running it. |`
2. `docs/archive/changes/README.md`, a section (or an existing online-services section if another branch added one):
   `| [2026-11-02-napple-social-cloud-2026-11-02-r9](2026-11-02-napple-social-cloud-2026-11-02-r9.md) | napple-social: friends, presence and invites service for Napple players (REST + WebSocket, SQLite). |`

I applied exactly these two lines in a scratch edit: `check_docs.py` then reports `checked 205 Markdown files: OK` (reverted before committing).
Also consider: `docs/reference/environment-variables.md` could list the `NAPPLE_SOCIAL_*` variables, and `docs/guides/network-providers.md` says
"This repository contains no online service implementation of any kind"; that sentence predates the Napple-run services and should be reworded to
"no Nintendo endpoints or protocol code" when the online services land.

## Tests run
`cargo test -p napple-social --locked` on Linux (rustc 1.97.0): **40 passed, 0 failed** (run 4 times, stable; about 3 s total).
`cargo clippy -p napple-social --all-targets`: no warnings. `cargo fmt -p napple-social` applied (repo `rustfmt.toml`).
- unit (11): JWT claims (good, expired, leeway, wrong/missing aud, bad sub/did, nbf, `HS256`/`none` refused, foreign signature, tampered payload),
  JWKS parsing, profile-list shapes, friend codes (round trip, every single-digit typo rejected, Luhn vector), config, rate-limit bucket, schema +
  unique codes + cascade, WAL on disk, privacy views.
- `tests/auth.rs` (9): healthz without auth; good token + profile; token failures over HTTP (missing, garbage, expired, clock passing exp, wrong aud,
  bad signature, unknown kid, `WWW-Authenticate`); JWKS cached, refreshed on unknown kid with 10 s throttle, rotation, revoked key refused after TTL;
  profile ownership (other account's profile 403, missing/invalid header 400); 60 s profile cache incl. new-profile re-fetch and deleted profile;
  account service down = 503; nickname snapshot refresh; rate limits (per account with Retry-After, per-profile friend requests).
- `tests/friends.rs` (8): codes (format, unique, regenerate + 3/day limit, input forms, typo, self); full request lifecycle with WebSocket events
  (send, duplicate, lists, wrong-party accept/cancel, accept, decline, cancel, crossed requests, remove); profiles on one account are separate;
  not accepting requests; blocks (self/unknown, ends friendship, idempotent, blocked sender sees `code_not_found`, blocker sees `blocked_by_you`,
  drops pending requests, unblock); 300-friend limit on send/receive/accept with rollback; 100 pending limit; `DELETE /me` erase.
- `tests/presence.rs` (6): friends-only presence with events, no event for unchanged state, sign-out; validation; heartbeats keep alive, 179 s still
  online, 181 s offline (reads and sweeper push), last seen = last heartbeat, late heartbeat 409; configurable interval; appear offline and nobody;
  list ordering.
- `tests/invites.rs` (6): invite over WebSocket and accept (`invite_response`, `invite_closed`); decline, replace, cancel, friends-only, self,
  message length, friend removal closes invites; 10-minute expiry incl. replay on late connect and sweeper `invite_closed`; block drops invites;
  stream protocol (no-credential upgrade refused, ping, heartbeat, invalid message, reauth same/other account/garbage, two devices); close 4001 on
  token expiry.

Release build smoke test: `NAPPLE_SOCIAL_BIND=127.0.0.1:18794 NAPPLE_ACCOUNT_URL=http://127.0.0.1:1 NAPPLE_SOCIAL_DATA_DIR=... target/release/napple-social`
answered healthz, created `social.sqlite3` + `-wal` + `-shm`, returned the JSON 401 for a bad token, and stopped cleanly on SIGTERM. Binary 11 MB.

## Run locally
```bash
cargo test -p napple-social
```
```bash
NAPPLE_ACCOUNT_URL=http://127.0.0.1:8790 NAPPLE_SOCIAL_DATA_DIR=/tmp/napple-social cargo run -p napple-social
```
```bash
curl -s http://127.0.0.1:8794/social/v1/healthz
```
Authenticated calls need a napple-account access token and `X-Napple-Profile: <profile uuid>`.

## Environment variables
| Variable | Default |
|---|---|
| `NAPPLE_SOCIAL_BIND` | `127.0.0.1:8794` (use `0.0.0.0:8794` in a container) |
| `NAPPLE_ACCOUNT_URL` | `https://pretend.napple.lat` |
| `NAPPLE_SOCIAL_DATA_DIR` | `./napple-social-data` |
| `NAPPLE_SOCIAL_HEARTBEAT_SECS` | `60` (offline after 3x) |
| `NAPPLE_SOCIAL_RATE_PER_MIN` | `240` per account |
| `NAPPLE_SOCIAL_IP_RATE_PER_MIN` | `600` per IP |
| `NAPPLE_SOCIAL_FRIEND_REQUESTS_PER_HOUR` | `30` per profile |
| `NAPPLE_SOCIAL_INVITES_PER_HOUR` | `120` per profile |
| `NAPPLE_SOCIAL_TRUST_PROXY` | off; `1` behind the ALB (uses the last `X-Forwarded-For` hop) |
| `NAPPLE_SOCIAL_LOG` | `info` |

## Assumptions about napple-account (please confirm when integrating)
- JWKS at `/account/v1/.well-known/jwks.json` with OKP/Ed25519 keys carrying `kid`; tokens carry the `kid` header and `alg` `EdDSA` (or `Ed25519`).
- `GET /account/v1/profiles` with the user's bearer token returns `{"profiles":[{"profile_id","nickname","icon_id"}]}` (a bare array and the
  aliases `id` / `name` / `icon` are also accepted). A 401 there is passed on as 401.
- Account and profile ids are UUIDs (any version accepted; canonical lowercase used everywhere).

## What the runtime / infra needs next
- **Deploy**: one container (single replica; live events are in-process) with a persistent volume for `NAPPLE_SOCIAL_DATA_DIR`, port 8794, routed at
  `https://pretend.napple.lat/social/v1/*` including WebSocket upgrade on `/social/v1/stream` (ALB idle timeout above 30 s; the server pings every
  30 s). Health check `GET /social/v1/healthz`. Set `NAPPLE_SOCIAL_TRUST_PROXY=1` behind the ALB. Back up with `sqlite3 ... ".backup"`. Scaling past
  one replica needs a shared event bus (e.g. Redis pub/sub) and a shared database first.
- **napple-account**: call `DELETE /social/v1/me` (with the user's token and the profile header) when a profile is deleted, or expose a
  profile-deleted webhook this service can consume; today a deleted profile's data stays until then (it loses access after the 60 s cache).
- **Runtime (`friend:u` provider)**: an `OnlineProvider` module that maps `friend_count` / `friend_list` to `GET /social/v1/friends` and
  `presence_update` to `PUT /social/v1/presence` (+ a 60 s heartbeat), keeps the WebSocket open for invites and presence, reauths before the 15-min
  token expiry, and hands invite `join_token`s to the napple-room / napple-online client. The `join_token` content is defined by those services.
- **Studio / launcher UI**: show the friend code, friend list with presence, requests, blocks, privacy toggles and incoming invites.
- **Docs**: the two index lines above.

## Integration result
- Date 2026-11-02, merged into main as 31448dd (head 73aa820, rebased by the orchestrator onto local main 21e785e). Conflicts: `Cargo.toml` members and
  `Cargo.lock` (main's side + `cargo build`: only napple-social's packages added). Outcome: **Small fix** (1) plus doc commits.
- Small fix `integrate: napple-social applies the reserved-name rules …` (the orchestration's Acceptance): `src/names.rs` is a copy of
  napple-account's `names.rs` (header says to change both together; a copy rather than a dependency so the service builds and deploys without the
  account crate) plus `check_text`. Presence `title_name` and `mode` get the full name rules (`napple` anywhere incl. look-alikes, the `nap-` prefix,
  invisible characters); an invite `message` gets the `napple` word and invisible-character rules only (the `nap-` prefix rule is for names; a message
  such as "nap time?" stays allowed). Rejections are 422 `reserved_name`; `unicode-normalization` added. Tests: presence (5 refused, 3 allowed) and
  invite cases; the names unit tests come with the copy. Social accounts carry no grants, so even the owner cannot put "napple" in these texts.
  **Decision for a human:** whether the prefix rule should also cover invite messages, and whether free text should honour account grants.
- Docs: guides index row, round r9 in the change archive (range to r9), the 422 row in the guide's error table. The "no online service" sentence in
  `network-providers.md` got an exception line when napple-online landed.
- `cargo test -p napple-social --locked` on macOS: 46 passed (40 + 6 names unit tests); clippy clean on the crate; `cargo fmt --check` clean.
- **JWT / JWKS / profiles against the real napple-account** (release binaries, dev mailer): a token from email sign-in is accepted (`/me` 200) through
  `/account/v1/.well-known/jwks.json` (OKP Ed25519, `kid`, `EdDSA`) and `/account/v1/profiles` (`{"profiles":[{"profile_id","nickname","icon_id"}]}`);
  a garbage token 401; presence `Online race` 200, `N4pple cup` 422, `nap-crew` 422. The file's assumptions about napple-account hold.
- Not done: deployment (infra has no social service yet), the runtime `friend:u` provider (online-friends, r10), napple-account calling
  `DELETE /social/v1/me` on profile deletion, `NAPPLE_SOCIAL_*` in `docs/reference/environment-variables.md`.
