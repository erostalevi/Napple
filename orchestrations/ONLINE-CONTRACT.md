# Napple Online contract (round r12; read by every r12 item)

Goal of the round: one scripted run: **create account -> sign in on a second device -> cloud save follows -> add a friend -> pick a visibility tier -> play MK8D online together**, all driven from the in-game overlay Online tab. No website, no messaging, no passkeys, no billing, no iOS in this round. Never contact Nintendo servers; no Nintendo credentials/keys/artwork anywhere.

## Hosts and env names (the deployed stack is `pretend.napple.lat`, Caddy in front; services listen on localhost)
- napple-account: `/account/v1/*`. napple-social: `/social/v1/*` (+ WebSocket `/social/v1/stream`). napple-online: existing (NEX over WebSocket, `/healthz`). napple-room: TCP 31415.
- Env (all optional, defaults for local dev in brackets): `NAPPLE_ACCOUNT_URL`, `NAPPLE_SOCIAL_URL`, `NAPPLE_ONLINE_SERVER` (client), `NAPPLE_ACCOUNT_BIND`, `NAPPLE_ACCOUNT_DATA_DIR`, `NAPPLE_MAIL_PROVIDER=dev|ses|smtp` [dev prints the mail], `NAPPLE_MAIL_FROM=no-reply@napple.lat`, `NAPPLE_SES_REGION=us-east-1`, `NAPPLE_SES_CONFIG_SET=napple-transactional`, `NAPPLE_SMTP_URL` (smtp provider), `NAPPLE_PHOTO_DIR` [data dir/photos] or `NAPPLE_PHOTO_BUCKET` (S3), `NAPPLE_ADMIN_EMAILS`, `NAPPLE_SOCIAL_BIND`, `NAPPLE_SOCIAL_DATA_DIR`.

## Accounts (napple-account)
- `POST /auth/signup {email, password, nickname, age_ok:true, terms_ok:true, device{name,platform,pubkey}}` -> 202; sends a 6-digit code (HTML + text template `crates/napple-account/templates/code_email.{html,txt}`); account is created unverified.
- `POST /auth/verify {email, code}` -> 200 tokens (access JWT Ed25519 15 min + rotating refresh 30 d) and the first profile (nickname from signup).
- `POST /auth/login {email, password, device}` -> tokens; unverified accounts get 403 `{reason:"unverified"}` and a fresh code.
- `POST /auth/password/reset/start {email}` -> always 202 (no enumeration); `POST /auth/password/reset/finish {email, code, new_password}`.
- Existing: refresh, logout, `GET /me`, devices, key request/grant/recovery (E2E save key), profiles, saves, account deletion (`DELETE /me`), JWKS. Passkeys and magic links are REMOVED.
- Password policy: >= 10 chars, not in the bundled common-password list, Argon2id (m=64 MiB, t=3, p=1). Codes: 6 digits, single use, 10 min, max 5 attempts. Limits: <= 5 code mails/hour/email and 20/hour/IP; 10 failed logins/15 min/account -> 15 min lock; generic error text for unknown email vs wrong password.
- Mail: provider trait (`ses` via SES v2 `SendEmail` with config set, `smtp`, `dev`); sends multipart text+HTML; bounces/complaints suppress the address (SES account-level suppression is ON) and the server marks it undeliverable.
- Terms: signup requires `age_ok` (13+) and `terms_ok`; stored with timestamps and the terms version.

## Profiles (napple-account; up to 8 per account; the profile is the in-game identity)
- Fields: `nickname` (3-20 chars `[A-Za-z0-9_.-]`, globally unique case-insensitively, reserved-name rules from ACCOUNT-CONTRACT.md), `bio` (<=160), `links` (<=3 of `{kind: discord|instagram|x|web, value}`; web must be https), `icon_id` (built-in 0..47), `photo` (optional), `tier` (1..5, default 1), `friend_code` (`NP-` + 12 digits, Luhn).
- Photo: `POST /profiles/{id}/photo` (multipart, auth). Limits: <= 5 MiB, <= 8192x8192 px, JPEG/PNG/WebP only by magic bytes (not file name/content-type), decode fully, strip ALL metadata, center-crop square, resize 256x256, re-encode JPEG q85, store privately; the original is never kept or served. `GET /profiles/{id}/photo` serves it subject to the tier rules (below). Any decode failure -> 422.

## Visibility tiers (per profile; enforced by napple-social, data from napple-account)
| Tier | Found by search/code | Friend requests | Profile / presence / stats seen by | In a shared lobby | Recent-players | Game friend requests |
|---|---|---|---|---|---|---|
| 1 Public (default) | everyone | anyone | everyone | real name+photo | appears | allowed |
| 2 Friends, searchable | everyone | anyone | friends (nickname+photo visible to all) | real | appears | allowed |
| 3 Friends, hidden | nobody | only from a shared lobby | friends | real | appears | only from shared lobby |
| 4 Private | nobody | nobody | friends | real | appears | blocked |
| 5 Ghost | nobody | nobody | nobody | bot alias, random per session, no profile | no trace | blocked |
The server ALWAYS keeps the real account behind every match (match log, 30 days) for reports/bans; blocks hide you from the blocked person at every tier. Tier changes apply immediately; existing friends stay friends (ghost: friends cannot see you either).

## Social (napple-social) — per PROFILE, JWT from napple-account (JWKS)
Existing API in `docs/guides/napple-social.md`. Additions: `GET /search?q=` (tier-aware, nickname prefix or friend code), `GET /profiles/{id}/public` (tier-filtered view), `POST /encounters {profile_ids, title_id, match_id}` (called by napple-online after a match; feeds recent-players), `GET /recent`, `POST /reports {profile_id, reason, match_id?}`, per-title presence `{title_id, mode, joinable_token}`, stats `POST /stats {title_id, deltas}` + `GET /profiles/{id}/stats` (tier-filtered). Reserved-name rule applies to presence/mode free text. No messaging.

## Online (napple-online)
Accepts the account access JWT (JWKS) instead of its own tokens; the game-visible principal derives from `profile_id`. Ghost: gives the game a random bot-style name per session (never the real nickname/photo). Writes the match log (real account, title, match id, participants, 30-day retention) and reports encounters and per-title stats to napple-social. MK8D protocol work (Pia/NAT/NEX) is in item mk8d-online-race.

## Client: credentials, mapping, C API (runtime + overlay)
- Shared credentials: `<NAPPLE_HOME>/online/credentials.json` (mode 0600; `NAPPLE_HOME` default `~/Library/Application Support/Napple`, so every Napple game on the Mac reads it): `{server, account_id, device_id, refresh_token_enc, device_key_enc}` wrapped with AES-256-GCM under a key stored in the macOS Keychain (service `lat.napple.online`, account `wrap-key`); if the Keychain is unavailable fall back to a 0600 key file next to it. Access tokens are memory-only.
- Profile mapping: `<NAPPLE_HOME>/online/profile_map.json` maps the local Switch user id -> Napple `profile_id`; default = the account's first profile; picked up automatically on first online launch (no linking step).
- Overlay API: header `frontend/include/napple_online_ui.h` (owned by item overlay-account; client-core implements it): a struct of function pointers provided by the runtime to the frontend (no link-time coupling): state query (`signed_in`, account email, current profile {id,nickname,tier,icon,photo path}, profiles[], devices[], pending device approvals, friends[] with presence, requests[], blocks[], recent[], per-title stats[], cloud-save status per title), async actions with a completion callback `(request_id, result_code, json)`: `signup`, `verify`, `login`, `logout`, `reset_start`, `reset_finish`, `switch_profile`, `create_profile`, `update_profile`, `upload_photo(path)`, `set_tier`, `friend_request`, `friend_accept/decline/remove`, `block/unblock`, `search`, `invite`, `join`, `report`, `approve_device/revoke_device`, `delete_account`; plus `set_change_callback`. Overlay screens use only this API; a stub implementation with canned data lives in the overlay item for UI work without servers.
- Cloud saves: existing client (`crates/napple-cloud`); this round only wires its status into the API above.

## Definition of done (item online-tieup verifies all of this, scripted)
1. Local servers up (`infra/local/run.sh` extended with social); signup with the dev mailer; verify code; first profile created; password login on a SECOND data dir (second device) incl. E2E key approval; save made on device A appears on B before game mount; add a friend by code between two accounts; tier changes alter search/presence results per the table; two MK8D instances race online through napple-online (or, if the race joiner is still open, report exactly how far they get); every step logged.
