# Napple Account + cloud saves: shared contract (round 8; read by napple-account, cloud-saves-client, napple-infra)

Decisions by the user: sign-in = **email magic link + passkeys (WebAuthn)**, no passwords stored; cloud saves = **end-to-end encrypted**.
Scope: Napple's own service at `https://pretend.napple.lat` (path prefix `/account/v1`). Never involves Nintendo accounts or servers. Only save data is
uploaded — never game content, keys or firmware.

## Identity
- Account: `account_id` (UUIDv7), verified email, display name, created/last-seen. Passkeys: WebAuthn credentials (RP ID `napple.lat`, origin
  `https://pretend.napple.lat` for the web flow; native apps use ASAuthorization with the associated domain `webcredentials:napple.lat`).
- Devices: each signed-in device has `device_id`, name, platform (macos/ios/cli), an **X25519 device public key**, created/last-seen, revocable.
- Tokens: Ed25519-signed JWT access tokens (15 min, `sub`=account_id, `did`=device_id, `aud`=napple) + opaque refresh tokens (30 days, rotated,
  stored hashed). JWKS at `/account/v1/.well-known/jwks.json` — napple-online verifies these to bind the online identity to the account.
- Switch user link: each account has up to 8 **Napple player profiles** (`profile_id`, nickname, icon id); a local acc user links to one. The online
  identity napple-online uses (its PID/principal) is derived from `profile_id`, so the same profile on Mac and iPhone is the same online player.

## Endpoints (JSON over HTTPS; all except auth start need `Authorization: Bearer`)
- `POST /auth/email/start {email}` -> sends a one-time link + 6-digit code (10 min). `POST /auth/email/finish {email, code, device{name,platform,pubkey}}` -> tokens.
- `POST /auth/passkey/register/{begin,finish}`, `POST /auth/passkey/login/{begin,finish}` (WebAuthn options/assertions; login also carries `device{...}`).
- `POST /auth/refresh {refresh_token}`, `POST /auth/logout`.
- `GET /me`, `PATCH /me {display_name}`, `GET/POST/PATCH/DELETE /profiles[/{id}]`, `GET /devices`, `DELETE /devices/{id}`.
- **Device approval (E2E)**: `GET /keys/requests` (pending new devices), `POST /keys/grant {device_id, wrapped_account_key}` — an existing device wraps the
  **account save key** (random 256-bit) to the new device's X25519 public key (HPKE base mode, X25519 + HKDF-SHA256 + AES-256-GCM). `GET /keys/mine` -> the
  wrapped key for this device. Recovery: a 24-word recovery code shown once at first setup; `POST /keys/recovery` stores the account key wrapped by a key
  derived from it (Argon2id) — the server never sees the code.
- **Saves**: per (profile_id, title_id) a linear history of versions. `GET /saves` (list: title, latest version, size, updated_at, device),
  `GET /saves/{title}/{profile}/versions`, `POST /saves/{title}/{profile}/versions {base_version, size, sha256_ciphertext}` -> `{version, upload_url}`
  (S3 presigned PUT, 15 min) — `409` with the current head if `base_version` is stale (client keeps both and asks the user), `POST .../versions/{v}/commit`,
  `GET .../versions/{v}` -> presigned GET. Quota 1 GiB per account (configurable), max 64 MiB per version, keep last 20 versions per title.
- Save blob = tar (sorted, deterministic) of the title's save directory for that user, compressed (zstd), encrypted with AES-256-GCM under a per-version
  key derived by HKDF from the account save key (info = `napple-save|title|profile|version`); header carries a format version and nonce. Server stores
  ciphertext only.

## Server storage
SQLite (WAL) in the instance data volume for accounts/devices/tokens/profiles/save metadata; S3 bucket `napple-saves-<account-id>` (SSE-S3,
block public access, lifecycle: abort incomplete uploads after 1 day, delete noncurrent objects after 7 days). Email via Amazon SES (domain `napple.lat`,
DKIM in Route 53, sender `no-reply@napple.lat`).

## Launch mode (user decision 2026-11-02): paid plans are OFF
Until the user announces the paid plan, **every account gets every feature for free** (the Plus column below, with Plus limits). Server config
`NAPPLE_PLANS_ENABLED=false` (the default) makes `plan.name = "free-launch"` with all feature flags on and Plus limits; no 402 is ever returned for plan
reasons, no upgrade prompts are shown, billing endpoints return 503. The engine below is built and tested but dormant; turning it on is a config change
(`NAPPLE_PLANS_ENABLED=true` + Stripe secrets) plus a grace period for existing users (config `NAPPLE_PLANS_GRACE_DAYS`, default 90).

## Plans and billing (dormant; user decision 2026-11-02, revised: free tier has no limits on online play)
Principles: online play and everything social are free and unlimited; Plus sells storage, history, sync and convenience — never access, never
gameplay advantage, never data hostage. All limits and feature flags come from server config (env/JSON), not code.

| Feature | Free | Napple Plus |
|---|---|---|
| Napple Account, passkeys, devices (up to 10), 8 profiles, shared online identity | yes | yes |
| Online play on napple-online (all modes), matchmaking, rankings | unlimited | unlimited |
| LDN / LAN Play rooms (public + password rooms) | unlimited | unlimited |
| Napple friends, presence, invites, room browser | yes | yes |
| Cloud saves (E2E) | 1 title, 5 versions, 256 MiB | unlimited titles, 30-day version history (min 20 versions/title), 10 GiB |
| Automatic session restore points (pre-launch snapshot, keep 3/title) | 1 title | all titles |
| Settings sync: per-game settings, controller maps, mod/cheat selections, Studio library metadata (no game files) | no | yes |
| Album sync: screenshots + short clips (E2E) | no | 5 GiB |
| Persistent private rooms (reserved name, invite list, stays up when empty) | no | 3 rooms |
| Beta update channel for Napple builds | no | yes |
| Supporter badge on the online profile (cosmetic only) | no | yes |
| Family: up to 4 accounts share one Plus | no | Family plan |

Prices (Stripe, configurable): **Plus 2.99 USD/month or 24.99 USD/year; Family 4.99 USD/month or 39.99 USD/year** (up to 4 accounts, each with
its own storage). Optional regional prices via Stripe price IDs. 14-day free trial of Plus once per account (no card needed; reverts to Free).
- Entitlements API: `GET /me` returns `plan {name, status, renews_at, trial_ends_at, features: {...flag: true|limit}}`. Every limited endpoint returns
  **402** `{reason: "plan_limit", feature, upgrade_url}`; clients keep data locally and show an upgrade prompt.
- Billing: Stripe Checkout + Customer Portal on a small web page at `/account/v1/billing/*`, signed webhooks, idempotent events; apps open the web
  page (no in-app payment). Family: owner invites up to 3 accounts by email; members see Plus while the owner's subscription is active.
- Free title switch: the free synced title can be changed once every 30 days.
- Never hold data hostage: downloads and restores always work; after a downgrade, data beyond Free limits stays read-only for 90 days, then is
  deleted after two email warnings. Account deletion cancels billing and deletes all data.
- Secrets at deploy time: `STRIPE_SECRET_KEY`, `STRIPE_WEBHOOK_SECRET`, `STRIPE_PRICE_PLUS_MONTHLY`, `STRIPE_PRICE_PLUS_YEARLY`,
  `STRIPE_PRICE_FAMILY_MONTHLY`, `STRIPE_PRICE_FAMILY_YEARLY`; without them everyone is Free (trial still works) and billing endpoints return 503.
