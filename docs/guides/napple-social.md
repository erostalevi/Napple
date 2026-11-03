# napple-social: friends and presence service

`crates/napple-social` is the Napple friends and presence service: Napple's own community service for Napple players, run by Napple at
`pretend.napple.lat`. It never talks to Nintendo and contains no Nintendo endpoints, tokens or protocol code. It gives every Napple player profile a
friend code, a friend list, presence ("playing <title>, joinable") and session invites, over REST JSON plus one WebSocket for live updates.

Identity comes from the Napple Account service (`napple-account`): it issues the access tokens and owns the accounts and their player profiles. The
social graph is per **profile** (the in-game identity), not per account: one account has up to 8 profiles and each has its own friends.

Related: the game side consumes it through the `friend:u` service of the runtime's [online glue](napple-online.md#friends-napple-social) on top of
the [network providers](network-providers.md); the overlay's Online tab through the [client core](napple-online-client.md).

## Run it

Linux or macOS, no system libraries (SQLite is bundled, TLS is rustls).

```bash
cargo run -p napple-social --release
```

```bash
NAPPLE_SOCIAL_BIND=0.0.0.0:8794 NAPPLE_ACCOUNT_URL=https://pretend.napple.lat NAPPLE_SOCIAL_DATA_DIR=/var/lib/napple-social ./target/release/napple-social
```

Tests (no network, no account service needed: they start a mock account service with a locally generated Ed25519 key):

```bash
cargo test -p napple-social
```

### Environment

| Variable | Default | Meaning |
|---|---|---|
| `NAPPLE_SOCIAL_BIND` | `127.0.0.1:8794` | Listen address. Use `0.0.0.0:8794` in a container. |
| `NAPPLE_ACCOUNT_URL` | `https://pretend.napple.lat` | Base URL of napple-account. JWKS: `${URL}/account/v1/.well-known/jwks.json`; profiles: `${URL}/account/v1/profiles`. |
| `NAPPLE_SOCIAL_DATA_DIR` | `./napple-social-data` | Holds `social.sqlite3` (+ `-wal`, `-shm`). Created if missing. |
| `NAPPLE_SOCIAL_HEARTBEAT_SECS` | `60` | Presence heartbeat interval; a profile goes offline after 3 missed heartbeats (180 s). Minimum 5. |
| `NAPPLE_SOCIAL_RATE_PER_MIN` | `240` | Authenticated requests (REST + WebSocket messages) per account per minute; burst is half. |
| `NAPPLE_SOCIAL_IP_RATE_PER_MIN` | `600` | Requests per client IP per minute, checked before authentication. |
| `NAPPLE_SOCIAL_FRIEND_REQUESTS_PER_HOUR` | `30` | Friend requests sent per profile per hour (also slows friend-code guessing). |
| `NAPPLE_SOCIAL_INVITES_PER_HOUR` | `120` | Invites sent per profile per hour. |
| `NAPPLE_SOCIAL_TRUST_PROXY` | off | `1`: take the client IP from the **last** `X-Forwarded-For` entry (set behind a load balancer that appends it). |
| `NAPPLE_SOCIAL_SERVICE_TOKEN` | unset | Credential napple-online sends as `X-Napple-Service` to `/encounters` and `/internal/*`. Unset: those endpoints answer 401. |
| `NAPPLE_ACCOUNT_SERVICE_TOKEN` | unset | Credential napple-social sends (header `x-napple-internal`; the same value as napple-account's `NAPPLE_INTERNAL_TOKEN`) to `GET ${NAPPLE_ACCOUNT_URL}/account/v1/internal/profiles/{id}` to learn other profiles' current tier. Unset: tiers come only from each owner's own calls. |
| `NAPPLE_SOCIAL_PROFILE_TTL_SECS` | `10` | How long a profile snapshot (tier, nickname, photo) is trusted before the directory is asked again. A failed lookup keeps the last snapshot. |
| `NAPPLE_SOCIAL_SEARCHES_PER_MIN` | `30` | `GET /search` per profile per minute. |
| `NAPPLE_SOCIAL_REPORTS_PER_HOUR` | `10` | Reports filed per profile per hour. |
| `NAPPLE_SOCIAL_LOG` | `info` | Log filter (`tracing` env-filter syntax, e.g. `debug`). Logs go to stdout, plain text; colour only on a terminal. |

## Authentication

Every endpoint except `/social/v1/healthz` needs two headers:

- `Authorization: Bearer <access token>`: a napple-account access token, a compact JWS signed with Ed25519 (`alg` `EdDSA`, also accepted: `Ed25519`)
  with a `kid` header. Claims used: `sub` = account id (UUID), `did` = device id, `aud` = `"napple"` (string or array), `exp` (15-minute tokens),
  optional `nbf`. 30 s clock leeway on `exp`/`nbf`.
- `X-Napple-Profile: <profile_id>`: which of the account's profiles is acting (UUID).

Keys come from the JWKS (`kty` `OKP`, `crv` `Ed25519`, `kid`, `x`; keys with `use` other than `sig` are ignored). The set is cached for 10 minutes; an
unknown `kid` triggers a re-fetch at most once per 10 s (so a rotation is picked up at once, and garbage `kid`s cannot hammer the account service).
After a successful re-fetch, a key no longer published is refused (revocation). While the account service is unreachable, already known keys keep working.

The profile check calls `GET ${NAPPLE_ACCOUNT_URL}/account/v1/profiles` with the caller's own token and caches the list per account for 60 s. A profile
missing from a cached list is re-fetched once the list is 5 s old (a profile just created on another device). The response may be
`{"profiles": [...]}` or a bare array; each item needs `profile_id` (or `id`) and may carry `nickname` (or `name`) and `icon_id` (or `icon`, string
or number). The nickname and icon are snapshotted into the social database on every call, so friends see the current ones.

A profile is registered (and gets its friend code) on its first authenticated call.

## Errors

Every error is `{"error": {"code": "...", "message": "..."}}`. 401s carry `WWW-Authenticate: Bearer ...`, 429s carry `Retry-After` (seconds).

| Status | Codes |
|---|---|
| 400 | `invalid_json`, `invalid_path`, `invalid_id`, `missing_profile`, `invalid_profile`, `invalid_friend_code`, `cannot_friend_self`, `cannot_block_self`, `cannot_invite_self`, `invalid_presence`, `invalid_invite`, `invalid_setting` |
| 401 | `missing_token`, `invalid_token`, `token_expired`, `wrong_audience`, `bad_signature`, `unknown_kid` |
| 403 | `profile_not_owned`, `not_accepting_requests`, `not_friends` (invites) |
| 404 | `not_found`, `profile_not_found`, `code_not_found`, `request_not_found`, `not_friends` (remove), `not_blocked`, `invite_not_found` |
| 409 | `request_exists`, `already_friends`, `blocked_by_you`, `friend_limit`, `target_friend_limit`, `too_many_pending`, `target_inbox_full`, `block_limit`, `presence_expired`, `not_joinable`, `too_many_invites` |
| 410 | `invite_expired` |
| 422 | `reserved_name`: presence `title_name` / `mode` break the reserved-name rules of [napple-account](napple-account.md#reserved-names-and-grants) ("napple" anywhere, look-alikes included; the `nap-` prefix; invisible characters), or an invite `message` contains "napple" or invisible characters (`src/names.rs`, a copy of napple-account's) |
| 429 | `rate_limited` |
| 503 | `account_unavailable` |

## REST API (`/social/v1`)

Times are Unix milliseconds (`*_ms`); ids are UUIDs (request and invite ids are UUIDv7). Bodies are JSON (max 16 KiB) and unknown fields are rejected.

Shared shapes:

```text
Profile  = {profile_id, nickname, icon_id}
Presence = {status: "online"|"playing"|"offline", title_id?, title_name?, mode?, joinable: bool, join_token?, last_seen_ms: int|null}
Friend   = {profile: Profile, friends_since_ms, presence: Presence}
Request  = {request_id, from: Profile, to: Profile, created_at_ms}
Invite   = {invite_id, from: Profile, to_profile_id, join_token, title_id, title_name, mode, message, created_at_ms, expires_at_ms}
```

| Method and path | Body | Answer |
|---|---|---|
| `GET /healthz` | | `{status: "ok", service, version, connections, server_time_ms}`, no auth |
| `GET /me` | | `{profile_id, account_id, device_id, nickname, icon_id, friend_code: "NP-1234-5678-9012", settings, friend_count, friend_limit: 300, presence}` (`presence` is your real state) |
| `DELETE /me` | | 204. Erases this profile's social data (friends, requests, blocks, invites, presence); the next call registers it afresh with a new code |
| `POST /me/friend-code/regenerate` | | `{friend_code}`; the old code stops working. 3 per day |
| `GET /settings` | | `{presence_visibility: "friends"|"nobody", appear_offline, accept_friend_requests}` |
| `PUT /settings` | any of those three fields | the settings; friends get a `presence` event if what they see changed |
| `GET /friends` | | `{count, limit: 300, friends: [Friend]}`, online first, then by nickname |
| `DELETE /friends/{profile_id}` | | 204; also closes live invites between the two |
| `GET /friend-requests` | | `{incoming: [Request], outgoing: [Request]}` |
| `POST /friend-requests` | `{friend_code}` | 201 `{status: "pending", request}`; or 200 `{status: "friends", friend}` when that profile had already asked you (crossed requests become a friendship) |
| `POST /friend-requests/{id}/accept` | | `{friend}` (recipient only) |
| `POST /friend-requests/{id}/decline` | | 204 (recipient only) |
| `DELETE /friend-requests/{id}` | | 204, cancel (sender only) |
| `GET /blocks` | | `{count, limit: 1000, blocks: [{profile, blocked_at_ms}]}` |
| `PUT /blocks/{profile_id}` | | `{blocked}`; idempotent. Ends the friendship and drops requests and invites in both directions |
| `DELETE /blocks/{profile_id}` | | 204; the friendship does not come back |
| `GET /presence` | | `{presence, as_friends_see_it, heartbeat_interval_secs, offline_after_secs}` |
| `PUT /presence` | `{status, title_id?, title_name?, mode?, join_token?}` | `{presence, heartbeat_interval_secs, offline_after_secs}`. Sets the state and counts as a heartbeat. `status` `offline` = `DELETE` |
| `POST /presence/heartbeat` | | same as PUT; 409 `presence_expired` when there is no live presence (then PUT the state again) |
| `DELETE /presence` | | 204, offline now (sign-out) |
| `GET /invites` | | `{incoming: [Invite], outgoing: [Invite]}` (live ones) |
| `POST /invites` | `{to_profile_id, join_token?, message?}` | 201 `{invite}`. Friends only. `join_token` defaults to your live presence's; title and mode come from your presence. A new invite to the same friend replaces the previous one |
| `POST /invites/{id}/accept` | | `{invite}` (with `join_token`); 410 after expiry |
| `POST /invites/{id}/decline` | | 204 |
| `DELETE /invites/{id}` | | 204, cancel (sender only) |

Presence rules: `playing` needs `title_id` (16 hex digits, stored upper case); `title_name` and `mode` up to 128 characters; `join_token` up to 1024
bytes, only with `playing`. `online` clears the title fields. `join_token` is opaque here: the game layer gets it from the Napple room / online
servers and hands it to friends, who give it back to those servers to join.

Friend codes: 12 digits, 11 random plus a Luhn check digit, shown as `NP-1234-5678-9012`. Input may omit the `NP` prefix, dashes and spaces; a code
that fails the check digit is a 400 without a lookup.

## WebSocket `/social/v1/stream`

Same two headers on the upgrade request. Server messages are JSON objects with a `type`:

| `type` | Fields | When |
|---|---|---|
| `hello` | `profile_id, friend_code, server_time_ms, heartbeat_interval_secs, token_expires_at` | first message |
| `invite` | `invite` | a friend invited you; also replayed on connect for every live invite you have |
| `invite_closed` | `invite_id, reason` (`expired`, `cancelled`, `replaced`, `accepted`, `declined`, `removed`) | an invite you sent or received is gone |
| `invite_response` | `invite_id, profile_id, accepted` | your invite was answered |
| `presence` | `profile_id, presence` | a friend's visible presence changed (heartbeats alone send nothing); also your own real state, to your other devices |
| `friend_request` | `request` | someone sent you a request |
| `friend_request_closed` | `request_id` | a request you sent or received is gone (accepted, declined, cancelled, blocked) |
| `friend_added` | `friend` | a new friendship (both sides, all devices) |
| `friend_removed` | `profile_id` | a friendship ended (removed, blocked, or the profile was erased) |
| `settings` | `presence_visibility, appear_offline, accept_friend_requests` | your settings changed |
| `pong`, `heartbeat_ack`, `reauthed`, `error` | | replies to client messages |

Client messages: `{"type":"ping"}`; `{"type":"heartbeat"}` (a presence heartbeat without a REST call); `{"type":"reauth","token":"<new access token>"}`
(same account; access tokens last 15 minutes, so clients reauth before `token_expires_at`). The socket closes with code **4001** when the token
expires (+30 s leeway) without a reauth, and **4008** when the client does not drain its queue (256 messages). The server pings every 30 s. Opening the
stream does not set presence; presence is explicit (`PUT /presence`).

## Privacy

- Presence is only ever shown to friends. There is no lookup of a stranger's presence.
- `presence_visibility: "nobody"`: friends see `offline` and `last_seen_ms: null`.
- `appear_offline: true`: friends see `offline` with the last time you were visibly online; heartbeats and changes while hidden leak nothing. Your
  own devices still see your real state (`GET /presence` shows both).
- `accept_friend_requests: false`: requests to your code get 403 `not_accepting_requests`.
- A profile that blocked you looks exactly like a wrong friend code (404 `code_not_found`). The blocker is told `blocked_by_you`.

## Limits

300 friends per profile (checked on send and accept, for both sides); 100 outgoing and 300 incoming pending requests; 1000 blocks; 100 live outgoing
invites; invites live 10 minutes; offline after 3 missed heartbeats; plus the rate limits above.

## Data model

SQLite in WAL mode (`synchronous=NORMAL`, foreign keys on), one connection behind a mutex: the service is one process and every operation is a few
indexed statements, so serialising them keeps limit checks race-free. `PRAGMA user_version` = 1.

| Table | Columns | Notes |
|---|---|---|
| `profiles` | `profile_id` PK, `account_id`, `friend_code` UNIQUE, `nickname`, `icon_id`, `presence_visibility`, `appear_offline`, `accept_requests`, `last_seen_ms`, `visible_last_seen_ms`, `created_ms`, `updated_ms` | registered on first use; nickname/icon are a snapshot from napple-account |
| `friendships` | `profile_a`, `profile_b` (PK, `a < b`), `created_ms` | one row per pair |
| `friend_requests` | `request_id` PK, `from_profile`, `to_profile` (UNIQUE pair), `created_ms` | pending only; accepted/declined/cancelled rows are deleted |
| `blocks` | `blocker`, `blocked` (PK), `created_ms` | |
| `presence` | `profile_id` PK, `status`, `title_id`, `title_name`, `mode`, `join_token`, `online_since_ms`, `last_heartbeat_ms` | a row only while online; deleted on sign-out or timeout |
| `invites` | `invite_id` PK, `from_profile`, `to_profile`, `join_token`, `title_id`, `title_name`, `mode`, `message`, `created_ms`, `expires_ms` | live only; answered/expired rows are deleted |

Every table references `profiles` with `ON DELETE CASCADE`, which is how `DELETE /me` erases a profile. A sweeper runs every 5 s: it times out
presence (pushing `offline` to friends), deletes expired invites (pushing `invite_closed`), and prunes the rate-limit and profile caches.

Code map: `src/auth.rs` (JWT, JWKS, profile check), `src/db.rs` (schema, queries, privacy views), `src/api.rs` (REST), `src/stream.rs` (WebSocket),
`src/hub.rs` (connections and events), `src/codes.rs` (friend codes), `src/ratelimit.rs`, `src/config.rs`, `src/lib.rs` (router, auth extractor,
sweeper, server). Tests: `tests/auth.rs`, `tests/friends.rs`, `tests/presence.rs`, `tests/invites.rs` (harness in `tests/common/mod.rs`).

## Visibility tiers, search, lobbies, recent players, reports, stats

Every profile has a tier 1..5 owned by napple-account (`tier` in its profile JSON). Social keeps a snapshot (refreshed by the owner's own calls and by
the service-credential directory lookup, TTL above) and applies the table below in `src/visibility.rs`, a thin adapter over
`crates/napple-visibility` (`can_view(viewer, tier, kind)`, the same pure function napple-account uses). A profile a viewer may not see answers exactly like a missing
one (404), and **a block hides both ways** at every tier.

| Tier | Search / friend code | Friend request | Card (nickname, icon, photo) | Presence, stats | Lobby | Recent |
|---|---|---|---|---|---|---|
| 1 Public | everyone | anyone | everyone | everyone | real | yes |
| 2 Friends, searchable | everyone | anyone | everyone | friends | real | yes |
| 3 Friends, hidden | nobody | shared lobby only | friends | friends | real | yes |
| 4 Private | nobody | nobody | friends | friends | real | yes |
| 5 Ghost | nobody | nobody | nobody | nobody (friends too) | bot alias | no trace |

Existing friendships survive a tier change; a ghost's friends see it offline with no title and get no presence events. `GET /me` also returns `tier` and `photo`.

| Method and path | Body / query | Answer |
|---|---|---|
| `GET /search?q=` | nickname prefix (>= 2 chars, case-insensitive) or friend code | `{results: [{profile, friend_status, can_friend_request}]}`, max 20, tiers 1-2 only, self and blocks excluded |
| `POST /friend-requests` | `{friend_code}` **or** `{profile_id, lobby_id?}` | as above; tiers 3-5 answer 404 `code_not_found` / `profile_not_found` unless the shared-lobby proof holds (tier 3 only) |
| `GET /profiles/{id}/public` | | `{profile, friend_status, can_friend_request, presence?, stats_visible?}`; presence only when the viewer may see details |
| `POST /stats` | `{title_id, deltas: {key: int}}` | own counters for the title (keys `[a-z0-9_.]` up to 48 chars, <= 16 per call, <= 32 per title, deltas within +-1e9) |
| `GET /profiles/{id}/stats?title_id=` | | `{profile_id, titles: {TITLE: {key: n}}}`, details-tier filtered |
| `GET /recent` | | `{recent: [{profile, title_id, match_id, last_played_ms, friend_status, can_friend_request}]}`, newest first, 100 max, 30 days |
| `GET /lobbies/{lobby_id}` | | members of a lobby the caller is in: real card, or `{ghost: true, alias}` for tier 5; blocked players are left out |
| `POST /reports` | `{profile_id, reason, match_id?}` or `{lobby_id, alias, reason, match_id?}` | 202 `{report_id}`; reason 1-500 chars; the alias form reports a ghost without learning who it is |

Presence also accepts `joinable_token` as an alias of `join_token`; the reserved-name rule covers `title_name` and `mode`.

**Shared lobby proof.** napple-online registers lobbies with the service credential; a friend request to a tier 3 profile is allowed when both are members of the
named live lobby (`lobby_id`), or of any live lobby, or met in a match in the last 24 h (`/encounters`). Tier 4 and 5 never accept requests.

Service endpoints (header `X-Napple-Service`, constant-time compare):

| Method and path | Body | Answer |
|---|---|---|
| `POST /encounters` | `{profile_ids: [2..64], title_id, match_id}` | `{recorded}`; records every non-ghost, non-blocked pair, one row per pair (latest wins); unknown ids ignored |
| `POST /internal/lobbies` | `{lobby_id, members: [{profile_id}], ttl_secs?}` (default 2 h, max 24 h) | `{lobby_id, expires_ms, members: [{profile_id, alias}]}`; aliases (`Bot-XXXX`) are random per lobby and stable while a member stays; use them as the in-game ghost name |
| `POST /internal/stats` | `{profile_id, title_id, deltas}` | the title's counters |
| `GET /internal/reports` | | latest 200 reports with the real profile and account on both sides (moderation) |

Profile source: `GET /account/v1/profiles` (caller's token) for the acting profile, and `GET /account/v1/internal/profiles/{id}` with `x-napple-internal` for
others. Tables added in schema 2: `stats`, `encounters`, `lobbies`, `reports` (and `tier`, `photo`, `synced_ms` on `profiles`); code in `src/social.rs`, tests in `tests/tiers.rs`.

## Operating notes

- One instance per database. Live updates are in-process; running several replicas needs a shared event bus first (and a shared database).
- Back up with `sqlite3 social.sqlite3 ".backup out.sqlite3"` (safe while running in WAL mode).
- The service holds no secrets: it only needs the public JWKS. It never sees passwords or save data.
