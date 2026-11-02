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

## Plans and billing: NOT BUILT (user decision 2026-11-02)
No billing, no Stripe, no plan engine in this round. Every account gets every feature. `GET /me` returns a static
`plan {name: "free", features: {}}` placeholder only so clients have a stable field later. Storage limits are plain server config
(`NAPPLE_SAVE_QUOTA_BYTES`, default 10 GiB per account; `NAPPLE_SAVE_VERSIONS`, default 20 per title; max 64 MiB per version), returning
413 `{reason: "quota"}` when exceeded. The paid-plan spec (Free/Plus/Family) is parked in `orchestrations/backlog.md` for later.

## Reserved names (user decision 2026-11-02)
Applies to every user-chosen name: account `display_name`, profile `nickname`, and any future username/handle or room name shown to others.
- Normalise before checking: Unicode NFKC + case-fold, remove whitespace, `-`, `_`, `.` and other punctuation for the "contains" check, and map common
  look-alikes to their skeleton (Unicode confusables: Cyrillic/Greek letters, fullwidth forms; leet: 0->o, 1/l/|/I->l, 4/@->a, 3->e, 5/$->s, 7->t, 9->g).
- **"napple" is forbidden anywhere** in a name (e.g. `Napple`, `xNAPPLEx`, `n4pple`, `nappIe`, `na pple`), except the single exact name `napple`
  (and profile nickname `napple`) granted to the owner's account.
- **Prefix `nap-` is reserved**: a name whose normalised form (before removing punctuation) starts with `nap-` / `nap_` / `nap ` / `nap.` (and look-alikes
  such as `n4p-`) is allowed only for accounts holding the `reserved_prefix` grant (approved contributors/members later).
- Grants are stored per account (`grants: ["owner_napple", "reserved_prefix"]`) and set only by an admin: bootstrap admin = the account whose verified
  email is in `NAPPLE_ADMIN_EMAILS`; admin endpoints `POST /admin/grants {account_id|email, grant}` / `DELETE ...` (admin JWT required) and a CLI
  subcommand of the server binary (`napple-account admin grant <email> reserved_prefix`). Rejected names return 422 `{reason: "reserved_name"}` with a
  human message; existing names are re-checked when renamed. Tests cover every rule above, including look-alikes.
