# Napple Account server

`crates/napple-account` (binary `napple-account`) is Napple's own account service at
`https://pretend.napple.lat/account/v1`: one identity across Mac, iPhone and the CLI, the end-to-end
encrypted save-key exchange, cloud-save version metadata with presigned S3 URLs, player profiles with
reserved-name rules, and admin grants. There are no paid plans this round (contract decision 2026-11-02):
every account gets every feature, storage limits are plain config. It never involves Nintendo accounts or servers, and it only ever stores **save
ciphertext**: never game content, keys or firmware. The shared contract is `orchestrations/ACCOUNT-CONTRACT.md`.
This guide is the API reference, the security and threat model, the data-retention rules and a local walkthrough.

## Run it locally

```bash
cargo build --release -p napple-account --bin napple-account
```

```bash
NAPPLE_ACCOUNT_DATA=$(mktemp -d) NAPPLE_ACCOUNT_PUBLIC_BASE=http://127.0.0.1:8082/account/v1 target/release/napple-account
```

With no AWS settings it runs fully offline: SQLite in the data dir, save blobs in `<data>/blobs` served
by the server itself under `/account/v1/dev-blobs/` (HMAC-signed, 15-minute URLs, same headers as S3), the **dev
mailer** (prints the sign-in email, code included, to stdout). A complete curl walkthrough (sign in, upload,
commit, conflict, reserved name, download, delete) is at the end of this guide.

Other commands: `napple-account rotate-jwt-key` (adds a signing key; restart to sign with it, the old keys stay
in the JWKS), `napple-account retention` (one cleanup pass), `napple-account admin grant|revoke <email> <grant>` and
`napple-account admin list <email>` (edit grants directly in the database; the account must have signed in once).

## Configuration (environment only)

| Variable | Default | Meaning |
|---|---|---|
| `NAPPLE_ACCOUNT_HTTP` | `127.0.0.1:8082` | listen address (container: `0.0.0.0:8082`, plain HTTP behind Caddy) |
| `NAPPLE_ACCOUNT_DATA` | `./napple-account-data` | SQLite (`account.sqlite3`, WAL), `jwt-keys.json` (0600), dev blobs |
| `NAPPLE_ACCOUNT_ENV` | (dev) | `production` refuses the dev mailer and the filesystem blob store |
| `NAPPLE_ACCOUNT_PUBLIC_BASE` | `https://pretend.napple.lat/account/v1` | used in links, URLs, the JWT `iss` |
| `NAPPLE_ACCOUNT_ISSUER` | = public base | JWT `iss` override |
| `NAPPLE_ACCOUNT_LINK_BASE` | `<public base>/auth/email/link` | where the magic link points (a universal-link page) |
| `NAPPLE_ACCOUNT_RP_ID` / `_RP_NAME` | `napple.lat` / `Napple` | WebAuthn relying party |
| `NAPPLE_ACCOUNT_WEBAUTHN_ORIGINS` | `https://pretend.napple.lat,https://napple.lat` | accepted `clientData.origin` values |
| `NAPPLE_ACCOUNT_CORS_ORIGINS` | `https://napple.lat,https://pretend.napple.lat,https://www.napple.lat` | browser origins allowed by CORS |
| `NAPPLE_ACCOUNT_TRUST_PROXY` | off | `1`: client IP = last `X-Forwarded-For` hop (set behind Caddy) |
| `NAPPLE_ACCOUNT_JWT_SEED` | (key file) | optional base64 32-byte Ed25519 seed instead of `jwt-keys.json` |
| `NAPPLE_ACCOUNT_MAILER` | `dev` | `ses` for Amazon SES v2 |
| `NAPPLE_ACCOUNT_MAIL_FROM` / `_SES_REGION` | `Napple <no-reply@napple.lat>` / `AWS_REGION` | SES sender and region |
| `NAPPLE_ACCOUNT_S3_BUCKET` | (unset = filesystem) | bucket for save ciphertext, e.g. `napple-saves-<aws-account-id>` |
| `NAPPLE_ACCOUNT_S3_REGION` | `AWS_REGION` or `us-east-1` | |
| `NAPPLE_ACCOUNT_S3_ENDPOINT` | (AWS) | S3-compatible endpoint, path-style (MinIO, tests) |
| `AWS_ACCESS_KEY_ID` / `_SECRET_ACCESS_KEY` / `_SESSION_TOKEN` | (IMDSv2) | otherwise the EC2 instance role via IMDSv2 (in Docker set the instance's `HttpPutResponseHopLimit` to 2) |
| `NAPPLE_SAVE_QUOTA_BYTES` | `10GiB` | storage per account (all titles and profiles, uploads in flight included) |
| `NAPPLE_SAVE_VERSIONS` | `20` | committed versions kept per (title, profile) |
| `NAPPLE_ADMIN_EMAILS` | (none) | comma-separated bootstrap admin emails (see "Reserved names and grants") |
| `NAPPLE_ACCOUNT_MAX_VERSION_BYTES` | `64MiB` | per save version |
| `NAPPLE_ACCOUNT_ACCESS_TTL` / `_REFRESH_TTL` / `_EMAIL_CODE_TTL` | `900` / `2592000` / `600` s | |
| `NAPPLE_ACCOUNT_AUTH_IP_PER_MIN` / `_EMAIL_PER_15MIN` | `30` / `5` | rate limits |

Health: `GET /healthz` and `GET /account/v1/healthz` -> `200 ok napple-account`. Logs are one line each on
stdout/stderr; SIGTERM shuts down gracefully.

## API

JSON over HTTPS, prefix `/account/v1`. Every endpoint except the auth start/finish ones, `healthz`, the JWKS,
and the HTML page needs `Authorization: Bearer <access token>`. Binary fields are base64url
without padding (standard base64 and padding are accepted on input). Times are unix seconds. Errors are
`{"error": "<code>", "message": "...", ...}`; some add `error_code`, `reason`, `field`, `head`.

### Sign-in and tokens

`device` everywhere is `{"name": "Neto's Mac", "platform": "macos" | "ios" | "cli", "pubkey": "<X25519 public key, 32 bytes>"}`.
Signing in again with the same device public key is the same device (it keeps its wrapped save key).

| Endpoint | Body -> answer |
|---|---|
| `POST /auth/email/start` | `{email}` -> `202 {sent, expires_in}`; mails a 6-digit code and a link `<link base>#email=…&token=…` (10 min, single use). Same answer whether or not the account exists. |
| `POST /auth/email/finish` | `{email, code \| token, device}` -> tokens (below), creating the account (and its first profile) on first use |
| `GET /auth/email/link` | HTML landing page: hands the fragment to `napple://auth/email#…` (the token never reaches a server log) |
| `POST /auth/passkey/register/begin` (auth) | -> `{challenge_id, publicKey: PublicKeyCredentialCreationOptions}` (resident key + UV required, `attestation: none`, ES256/EdDSA) |
| `POST /auth/passkey/register/finish` (auth) | `{challenge_id, name?, credential: PublicKeyCredential JSON}` -> `201 {credential_id, name}` |
| `POST /auth/passkey/login/begin` | -> `{challenge_id, publicKey: PublicKeyCredentialRequestOptions}` (discoverable: empty `allowCredentials`) |
| `POST /auth/passkey/login/finish` | `{challenge_id, credential, device}` -> tokens |
| `POST /auth/refresh` | `{refresh_token}` -> new tokens (rotation) |
| `POST /auth/logout` (auth) | ends this device's sessions (`204`); the device and its key stay |
| `GET /passkeys`, `DELETE /passkeys/{credential_id}` | manage passkeys |

Tokens: `{access_token, token_type: "Bearer", expires_in: 900, refresh_token, refresh_expires_in, account_id, device_id, key_state: "granted" | "pending" | "bootstrap", new_account}`.

The access token is an EdDSA (Ed25519) JWT: header `{"alg":"EdDSA","typ":"JWT","kid"}`, claims `iss`
(= public base), `aud: "napple"`, `sub` (account_id), `did` (device_id), `sep` (device session epoch), `prf`
(the account's profile ids, at most 8), `iat`, `exp`, `jti`. **napple-online** verifies it with
`GET /.well-known/jwks.json` (RFC 8037 OKP keys; cache 5 minutes; refetch on an unknown `kid`), checks `alg`,
`aud`, `iss`, `exp`, and maps a `profile_id` from `prf` to its online identity. `jwt::verify_with_jwks` in the
crate is the reference verifier. A revoked or logged-out device's access token stays cryptographically valid
until it expires (≤ 15 min); this server rejects it immediately (it checks `sep`), other services accept that window.

Refresh tokens are 256-bit random, stored as SHA-256 hashes, valid 30 days, rotated on every use. Presenting
a rotated-out token again is treated as theft: every session of that device ends (refresh tokens deleted,
`sep` bumped). Clients must therefore serialize refreshes; if a refresh answer is lost, the client signs in
again (same device key, so it keeps its save key).

### Account, profiles, devices

| Endpoint | |
|---|---|
| `GET /me` | `{account_id, device_id, email, display_name, created_at, last_seen, key_id, has_recovery, key_state, grants, plan, storage}` |
| `PATCH /me` | `{display_name}` (1-32 chars, reserved-name rules) |
| `DELETE /me` | `{confirm_email}` -> `204`. Deletes every save object and every row. Irreversible. |
| `GET /profiles`, `POST /profiles`, `GET/PATCH /profiles/{id}`, `DELETE /profiles/{id}[?delete_saves=true]` | see "Profiles" below |
| `GET /devices`, `DELETE /devices/{id}` | revoke = sign out everywhere + drop its wrapped key |

`plan` is the static placeholder `{"name": "free", "features": {}}` (no paid plans this round; clients keep the
field for later). `storage` is `{quota, used, versions_per_title, max_version_bytes}` from server config.

### Profiles

Up to 8 per account (the first is created with the account); the profile is the in-game identity.

| Field | Rule |
|---|---|
| `nickname` | 3-20 chars of `[A-Za-z0-9_.-]`, **globally unique ignoring case** (`409 nickname_taken`), reserved-name rules (`422 reserved_name`) |
| `bio` | <= 160 chars, no control or invisible characters (newline allowed) |
| `links` | <= 3 of `{kind: discord\|instagram\|x\|web, value}`; `web` must be an `https://` URL (no credentials, ASCII host with a dot), handles are 2-32 of `[A-Za-z0-9_.]` (a leading `@` is dropped) |
| `icon_id` | built-in icon 0..47 |
| `photo` | `null` or `{url, updated_at}` (see below) |
| `tier` | 1..5 visibility tier, default 1 (the table is in `orchestrations/ONLINE-CONTRACT.md`; the rules are the pure function `napple_visibility::can_view`, shared with napple-social) |
| `friend_code` | `NP-` + 12 digits, last one a Luhn check digit; assigned automatically (also to profiles inserted by other code paths: SQLite trigger), stable, unique |

`POST /profiles {nickname, icon_id?, bio?, links?, tier?}`; `PATCH /profiles/{id}` takes any subset of
`nickname, bio, links, icon_id, tier` (all or nothing). Tier changes apply immediately.

**Photo.** `POST /profiles/{id}/photo` (multipart; the first part is the photo, its name/file name/content type are
ignored; 20/hour/account). Limits: <= 5 MiB (`413 photo_too_large`), <= 8192x8192 px (`422 photo_dimensions`),
JPEG/PNG/WebP decided by magic bytes only (`422 unsupported_type`: SVG, GIF, HEIC, AVIF, BMP, ... never reach a
decoder), empty file `422 empty_photo`, anything that does not decode fully (truncated, corrupt, JPEG without EOI)
`422 decode_failed`. The image is decoded completely, EXIF orientation applied, centre-cropped square, resized to
256x256, composited on white if transparent and **re-encoded as JPEG q85 from pixels**: EXIF/GPS, ICC, XMP, comments,
PNG text chunks and anything appended after the image end are gone, and the original is never stored. Decodes run on
the blocking pool, two at a time. `DELETE /profiles/{id}/photo` removes it; deleting the profile or the account
removes the object too.
`GET /profiles/{id}/photo` serves `image/jpeg` (`Cache-Control: private`, `nosniff`) to the owner, and to other
signed-in accounts only when `can_view(.., NicknameAndPhoto)` holds without friend data (tiers 1-2); anything hidden
answers `404`, indistinguishable from "no photo". Friends (tiers 3-4) are served by napple-social through the internal
route below.

Storage: `NAPPLE_PHOTO_DIR` (default `<data dir>/photos`, files 0600) or, when `NAPPLE_PHOTO_BUCKET` is set, a
**private** S3 bucket (`NAPPLE_PHOTO_REGION` or `NAPPLE_ACCOUNT_S3_REGION`, default us-east-1; optional
`NAPPLE_PHOTO_ENDPOINT` for S3-compatible stores). The server signs its own requests; objects are never presigned to
clients. Keys are `photos/<account>/<profile>/<random>`. Both backends implement `photos::PhotoStore`.

**Internal lookups for napple-social / napple-online.** Set `NAPPLE_INTERNAL_TOKEN` (>= 16 chars; without it these
routes answer 404) and send it as `x-napple-internal`: `GET /internal/profiles/{id}` (the full profile plus
`account_id`), `GET /internal/profiles/by-code/{NP-...}`, `GET /internal/profiles?prefix=ab&limit=20` (nickname
prefix, case-insensitive), `GET /internal/profiles/{id}/photo` (raw stored JPEG, no tier rules: the caller applies
`can_view`). Route them only on the loopback/private network, never through the public proxy.

### End-to-end save key

| Endpoint | |
|---|---|
| `GET /keys/requests` | devices of this account still waiting for the key: `{device_id, name, platform, pubkey, created_at}` |
| `POST /keys/grant` | `{device_id, wrapped_account_key, key_id?}`. Granting another device requires the caller to hold the key; any device may store its own copy (first-device bootstrap, or after recovery). `key_id` is optional; once a grant carried one, a different one is `409 key_mismatch`. |
| `GET /keys/mine` | `{key_id, wrapped_account_key, granted_by, granted_at}` or `404 {error_code: "key_pending", key_state}` |
| `POST /keys/recovery` | the client's recovery record, any JSON object ≤ 4 KiB (key holders only; an optional `key_id` must match) |
| `GET /keys/recovery` | the stored record, verbatim, or `404` |

Recommended formats (the server treats them as opaque, ≤ 1 KiB wrapped key, ≤ 4 KiB recovery record):
`wrapped_account_key` = HPKE base mode (RFC 9180) with X25519-HKDF-SHA256 / HKDF-SHA256 / AES-256-GCM to the
device's X25519 key, `info = "napple-account-key|<account_id>|<device_id>"`, empty AAD, encoded as
`enc (32 bytes) || ciphertext (48 bytes)` (`tests/keys.rs` does exactly this with the `hpke` crate).
`key_id` = a fingerprint of the account key the clients agree on (for example base64url of the first 16 bytes
of `HKDF-SHA256(key, info="napple-key-id")`). The cloud-saves client's recovery record is
`{kdf: "argon2id", m_kib, t, p, salt, wrapped}`.

### Cloud saves

Per (title, profile) a linear history of versions, numbered from 1.

| Endpoint | |
|---|---|
| `GET /saves` | latest version per (title, profile): `{title_id, profile_id, latest_version, size, updated_at, device_id, device_name}` |
| `DELETE /saves/{title}` | deletes the title's cloud saves (all profiles) |
| `GET /saves/{title}/{profile}/versions` | history, newest first: `{version, size, sha256_ciphertext, committed_at, created_at, device_id, device_name}` |
| `POST /saves/{title}/{profile}/versions` | `{base_version, size, sha256_ciphertext (hex)}` -> `201 {version, upload_id, upload_url, upload_method: "PUT", upload_headers, expires_at}` |
| `POST /saves/{title}/{profile}/versions/{v}/commit` | optional `{upload_id}` -> the new head |
| `GET /saves/{title}/{profile}/versions/{v}` | the version + `url` (= `download_url`, presigned GET, 15 min) |

Upload: `PUT upload_url` with exactly the `upload_headers` (`content-type: application/octet-stream`,
`x-amz-checksum-sha256`, and the `content-length` the HTTP client sets from the body). All three are signed: a
PUT without the checksum header is refused by S3. The URL signs
Content-Length, Content-Type and the SHA-256 checksum, so S3 itself rejects any other body. Commit then checks
the object exists with the declared size.

Status codes: `409 version_conflict` with `head` (the head version object, `head.version` its number) when
`base_version` is not the current head (at create or at commit, whichever loses the race); `413` with
`reason: "quota"` when the account's storage is full and `reason: "too_large"` above 64 MiB; `410 upload_expired`
for a pending upload older than a day; `400 upload_missing` / `upload_mismatch` at commit. A new version is
always `base_version + 1`. Committing prunes the oldest versions beyond `NAPPLE_SAVE_VERSIONS`. A device's new
upload for the same save replaces its own unfinished one. Any number of titles can sync.

### Reserved names and grants

Every name other players see (account `display_name`, profile `nickname`; later handles and room names, through
`names::check_name`) is checked:
- **"napple" anywhere is refused** (`Napple`, `xNAPPLEx`, `n4pple`, `nappIe`, `na pple`, `nаpple` with a Cyrillic
  а, fullwidth or math-styled letters, accents), except the exact name `napple` (any case) on an account with
  the `owner_napple` grant.
- **The `nap-` prefix is reserved**: names starting with `nap-`, `nap_`, `nap `, `nap.` (and look-alikes such as
  `n4p-`, `nap–` with an en dash) need the `reserved_prefix` grant.
- Normalisation: NFKD, combining marks dropped, a confusables table (Cyrillic, Greek and Latin look-alikes,
  dash and dot variants), leetspeak (0→o, 1/l/|/I→l, 4/@→a, 3→e, 5/$→s, 7→t, 9→g), lower-case; the "contains"
  check then ignores everything that is not a letter or digit. Invisible characters (zero-width spaces, bidi
  controls, soft hyphen) are refused outright (400); ZWJ stays allowed for emoji.
- A rejected name is `422 {error: "reserved_name", reason: "reserved_name", field, message}`. A new account
  whose email local part is reserved starts as `Player`. Removing a grant keeps names already chosen; the next
  rename is checked again.

Grants (`owner_napple`, `reserved_prefix`, `admin`) are per account and only admins set them. Admins are the
accounts whose (verified) email is in `NAPPLE_ADMIN_EMAILS`, plus accounts holding `admin`.

| Endpoint (admin token) | |
|---|---|
| `POST /admin/grants` | `{account_id \| email, grant}` -> `{account_id, grants}` |
| `DELETE /admin/grants` | same body, removes the grant |
| `GET /admin/grants?email=…` or `?account_id=…` | `{account_id, grants}` |

From the server host: `napple-account admin grant owner@napple.lat owner_napple`.

## Security model

- **No passwords.** Sign-in is an emailed one-time code / link (10 min, single use, 5 attempts per code, at most
  20 guesses per address per hour, codes stored as HMAC, link tokens as SHA-256) or a passkey (user verification
  required, origin + RP ID + challenge + counter checked, ES256/EdDSA, pure-Rust verifier in `src/webauthn.rs`).
- **Tokens.** 15-minute Ed25519 JWTs, 30-day rotated refresh tokens stored hashed, reuse detection, per-device
  session epochs so logout and revoke take effect at once on this server.
- **End-to-end saves.** The server never sees the account save key, the recovery code, or save plaintext. It
  stores public keys, wrapped keys, the recovery blob, and ciphertext with its size and hash.
- **Abuse limits.** Per-IP limit on every auth endpoint (30/min), per-email limits on start (5/15 min) and finish;
  64 KiB JSON bodies, 64 MiB per save version, a per-account quota (10 GiB default), uploads pinned to
  `application/octet-stream` with their length and SHA-256.
- **Names.** Reserved-name rules against impersonating Napple; admin-only grants for the exceptions.
- **Web.** CORS limited to the napple.lat origins; HTML pages set `no-referrer`; the link token lives in the URL
  fragment. Constant-time compares for codes, tokens, signatures and challenges.
- **Logs.** No tokens, codes, keys, signatures, presigned query strings or email addresses (account ids only).
  The dev mailer is the exception by design and `NAPPLE_ACCOUNT_ENV=production` refuses it.
- **Never game content.** Only save ciphertext is accepted, with size limits and the save content type; there is
  no endpoint for anything else.

## Threat model

| Threat | Mitigation | Residual risk |
|---|---|---|
| Stolen email inbox | the attacker can sign in and appears as a **pending device**; they get no save key unless a user approves them on an existing device | users may approve without looking: clients must show the device name, platform and time, and ask for confirmation |
| Phishing site relays codes | codes are short-lived and single use; passkeys are origin-bound and cannot be phished | email codes can still be relayed in real time; recommend passkeys |
| Stolen refresh token | rotation + reuse detection ends the device's sessions | a thief who uses it first holds the session until the user revokes the device |
| Stolen access token | 15-minute lifetime; revoke/logout end it here immediately | other services (napple-online) accept it until `exp` |
| Lost or stolen device | `DELETE /devices/{id}` signs it out and drops its wrapped key | the device may still have the account key in memory/Keychain; rotating the account key (re-encrypting every save) is a client feature, not done yet |
| Compromised server or database | only ciphertext, public keys and wrapped keys are stored; refresh tokens and codes are hashed | the attacker sees metadata (titles, sizes, times, device names, emails) and could deny service or delete data; they could also mark an attacker device "granted" but cannot wrap the real key for it |
| Malicious server pushes a bad key | `key_id` is pinned and clients should verify `key_id` against the key they unwrap | a server could hide versions or serve old ones (rollback); clients keep local copies and compare version numbers |
| Brute force | per-code attempts, per-email and per-IP limits | distributed brute force of 6-digit codes is bounded by 20 guesses/hour/address (~0.05%/day) |
| Two first devices race to create the account key | the first `POST /keys/grant` pins `key_id`; the second gets `409 key_mismatch` and must fetch the granted key instead | |
| Impersonating Napple staff by name | reserved-name rules with look-alike normalisation; grants only from admins | a look-alike missing from the confusables table; names other players see should still carry the account's grants as a badge in the UI |
| Admin account takeover | admins are few, named by email in config; grants only change names | a stolen admin email inbox can mint grants: admins should use passkeys |
| Using the bucket as free file hosting | per-version max size, quotas, content type pinned in the signed URL, short URL lifetimes | ciphertext cannot be inspected, by design |

## Data retention and deletion

- **Account deletion** (`DELETE /me` with the email typed as confirmation): queues every save object for
  deletion and deletes it right away, then removes the account and, through foreign keys, all devices, tokens,
  passkeys, profiles, grants and save metadata. Signing up again with the same email creates a new account.
  Objects that S3 could not delete at that moment stay in the deletion queue (object keys only, no personal
  data) and are retried every hour.
- **Hourly retention job**: pending uploads older than a day (and their objects), expired codes, ceremonies and
  refresh tokens, the deletion queue.
- **Versions**: the newest `NAPPLE_SAVE_VERSIONS` per (title, profile); older ones are deleted on the next commit.
- **S3 bucket** (napple-infra): SSE-S3, block public access, versioning with "delete noncurrent versions after 7
  days" and "abort incomplete multipart uploads after 1 day". Deleted saves are therefore gone from S3 within 7 days.
- **Logs** contain account and device ids, never emails or secrets.

## Deployment notes (for napple-infra)

- Image: `cargo build --release --locked -p napple-account --bin napple-account`; no system libraries beyond libc
  (rustls + ring, bundled SQLite). Read-only container, uid 10001, writes only under `NAPPLE_ACCOUNT_DATA`.
- Caddy: `handle /account/* { reverse_proxy account:8082 }` **without** stripping the prefix, ahead of the catch-all
  that goes to napple-online; set `NAPPLE_ACCOUNT_TRUST_PROXY=1`.
- Env: `NAPPLE_ACCOUNT_ENV=production`, `NAPPLE_ACCOUNT_MAILER=ses`, `NAPPLE_ACCOUNT_S3_BUCKET=napple-saves-<aws-account-id>`,
  `AWS_REGION`, `NAPPLE_ADMIN_EMAILS`. Instance role: `s3:PutObject`, `s3:GetObject` (also
  authorizes HEAD) and `s3:DeleteObject` on `arn:aws:s3:::napple-saves-*/saves/*` and `ses:SendEmail` from `no-reply@napple.lat`.
  IMDSv2 hop limit 2 for containers.
- Back up `account.sqlite3` and `jwt-keys.json` (losing the key file signs everybody out; nothing else).

## Walkthrough (curl, local)

Start the server as in "Run it locally" with `NAPPLE_ACCOUNT_PUBLIC_BASE=http://127.0.0.1:8082/account/v1`, then:

```bash
API=http://127.0.0.1:8082/account/v1
curl -s -X POST $API/auth/email/start -H 'content-type: application/json' -d '{"email":"me@example.com"}'
# the dev mailer prints "Your Napple sign-in code is NNNNNN" in the server's output
PUB=$(head -c 32 /dev/urandom | base64 | tr '+/' '-_' | tr -d '=')
curl -s -X POST $API/auth/email/finish -H 'content-type: application/json' \
  -d "{\"email\":\"me@example.com\",\"code\":\"NNNNNN\",\"device\":{\"name\":\"My Mac\",\"platform\":\"macos\",\"pubkey\":\"$PUB\"}}"
AT=<access_token from the answer>
curl -s $API/me -H "authorization: Bearer $AT"
P=$(curl -s $API/profiles -H "authorization: Bearer $AT" | python3 -c 'import json,sys;print(json.load(sys.stdin)["profiles"][0]["profile_id"])')
printf 'pretend ciphertext' > blob; SHA=$(shasum -a 256 blob | cut -d' ' -f1)
curl -s -X POST $API/saves/01006BB00C6F0000/$P/versions -H "authorization: Bearer $AT" -H 'content-type: application/json' \
  -d "{\"base_version\":0,\"size\":18,\"sha256_ciphertext\":\"$SHA\"}"
# PUT the file to upload_url with the upload_headers, then:
curl -s -X POST $API/saves/01006BB00C6F0000/$P/versions/1/commit -H "authorization: Bearer $AT"
curl -s $API/saves -H "authorization: Bearer $AT"
```

The scripted version is `crates/napple-account/scripts/walkthrough.sh` (all of the above plus the 409 and 422
cases, a second title, the download check and account deletion; `BIN=<path to napple-account>`, runs on port 18082 in a temp
dir and cleans up). Its 2026-11-02 output is in `docs/archive/changes/2026-11-02-claude-napple-account-2026-11-02-r8.md`.
