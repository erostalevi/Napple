# Napple Account server

`crates/napple-account` (binary `napple-account`) is Napple's own account service at
`https://pretend.napple.lat/account/v1`: one identity across Mac, iPhone and the CLI, the end-to-end
encrypted save-key exchange, cloud-save version metadata with presigned S3 URLs, and the Free / Napple Plus
plans billed through Stripe. It never involves Nintendo accounts or servers, and it only ever stores **save
ciphertext**: never game content, keys or firmware. The shared contract is `orchestrations/ACCOUNT-CONTRACT.md`.
This guide is the API reference, the security and threat model, the data-retention rules and a local walkthrough.

## Run it locally

```bash
cargo build --release -p napple-account --bin napple-account
```

```bash
NAPPLE_ACCOUNT_DATA=$(mktemp -d) NAPPLE_ACCOUNT_PUBLIC_BASE=http://127.0.0.1:8082/account/v1 target/release/napple-account
```

With no AWS or Stripe settings it runs fully offline: SQLite in the data dir, save blobs in `<data>/blobs` served
by the server itself under `/account/v1/dev-blobs/` (HMAC-signed, 15-minute URLs, same headers as S3), the **dev
mailer** (prints the sign-in email, code included, to stdout), and billing off (every account on Free,
`/billing/*` returns 503). A complete curl walkthrough (sign in, upload, commit, conflict, 402, download,
delete) is at the end of this guide.

Other commands: `napple-account rotate-jwt-key` (adds a signing key; restart to sign with it, the old keys stay
in the JWKS), `napple-account retention` (one cleanup pass).

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
| `STRIPE_SECRET_KEY`, `STRIPE_WEBHOOK_SECRET`, `STRIPE_PRICE_MONTHLY`, `STRIPE_PRICE_YEARLY` | (unset) | all four or none; none = billing off |
| `STRIPE_API_BASE` | `https://api.stripe.com` | for mocks |
| `NAPPLE_PLAN_FREE_TITLES` / `_VERSIONS` / `_QUOTA` | `1` / `10` / `256MiB` | Free plan |
| `NAPPLE_PLAN_PLUS_TITLES` / `_VERSIONS` / `_QUOTA` | `0` (unlimited) / `20` / `5GiB` | Napple Plus |
| `NAPPLE_PLAN_TITLE_SWITCH_DAYS` / `_READONLY_DAYS` / `_DELETION_WARNING_DAYS` | `30` / `90` / `7` | |
| `NAPPLE_ACCOUNT_MAX_VERSION_BYTES` | `64MiB` | per save version |
| `NAPPLE_ACCOUNT_ACCESS_TTL` / `_REFRESH_TTL` / `_EMAIL_CODE_TTL` | `900` / `2592000` / `600` s | |
| `NAPPLE_ACCOUNT_AUTH_IP_PER_MIN` / `_EMAIL_PER_15MIN` | `30` / `5` | rate limits |

Health: `GET /healthz` and `GET /account/v1/healthz` -> `200 ok napple-account`. Logs are one line each on
stdout/stderr; SIGTERM shuts down gracefully.

## API

JSON over HTTPS, prefix `/account/v1`. Every endpoint except the auth start/finish ones, `healthz`, the JWKS,
the HTML pages and the Stripe webhook needs `Authorization: Bearer <access token>`. Binary fields are base64url
without padding (standard base64 and padding are accepted on input). Times are unix seconds. Errors are
`{"error": "<code>", "message": "...", ...}`; some add `error_code`, `reason`, `head`, `upgrade_url`.

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
| `GET /me` | `{account_id, device_id, email, display_name, created_at, last_seen, key_id, has_recovery, key_state, plan}` |
| `PATCH /me` | `{display_name}` (1-32 chars) |
| `DELETE /me` | `{confirm_email}` -> `204`. Deletes the Stripe customer (cancels the subscription), every save object and every row. Irreversible. |
| `GET /profiles`, `POST /profiles {nickname, icon_id}`, `GET/PATCH /profiles/{id}`, `DELETE /profiles/{id}[?delete_saves=true]` | up to 8 profiles; delete refuses (`409 profile_has_saves`) while the profile has cloud saves unless `delete_saves=true` |
| `GET /devices`, `DELETE /devices/{id}` | revoke = sign out everywhere + drop its wrapped key |

`plan` is `{name: "free" | "plus", display_name, status, interval, renews_at, cancel_at_period_end, synced_titles_limit (null = unlimited), versions_per_title, quota, used, billing_available, upgrade_url}`.

### End-to-end save key

| Endpoint | |
|---|---|
| `GET /keys/requests` | devices of this account still waiting for the key: `{device_id, name, platform, pubkey, created_at}` |
| `POST /keys/grant` | `{device_id, wrapped_account_key, key_id}`. Granting another device requires the caller to hold the key; any device may store its own copy (first-device bootstrap, or after recovery). The first grant pins the account's `key_id`; a different one is `409 key_mismatch`. |
| `GET /keys/mine` | `{key_id, wrapped_account_key, granted_by, granted_at}` or `404 {error_code: "key_pending", key_state}` |
| `POST /keys/recovery` | `{recovery_blob, kdf, key_id}` (key holders only) |
| `GET /keys/recovery` | the stored `{recovery_blob, kdf, key_id, updated_at}` |

Recommended formats (the server treats them as opaque bytes, ≤ 1 KiB wrapped key, ≤ 4 KiB recovery blob):
`wrapped_account_key` = HPKE base mode (RFC 9180) with X25519-HKDF-SHA256 / HKDF-SHA256 / AES-256-GCM to the
device's X25519 key, `info = "napple-account-key|<account_id>|<device_id>"`, empty AAD, encoded as
`enc (32 bytes) || ciphertext (48 bytes)` (`tests/keys.rs` does exactly this with the `hpke` crate).
`key_id` = a fingerprint of the account key the clients agree on (for example base64url of the first 16 bytes
of `HKDF-SHA256(key, info="napple-key-id")`). `kdf` = `{"alg":"argon2id","salt":…,"m":…,"t":…,"p":…}`.

### Cloud saves

Per (title, profile) a linear history of versions, numbered from 1.

| Endpoint | |
|---|---|
| `GET /saves` | latest version per (title, profile): `{title_id, profile_id, latest_version, size, updated_at, device_id, device_name, read_only, delete_after}` |
| `GET /saves/titles` | `{synced_titles_limit, next_switch_at, titles: [{title_id, state: "active" \| "readonly", enabled_at, readonly_since, delete_after}]}` |
| `POST /saves/titles` | `{title_id}`: enable a title (Plus) or switch the Free title (the old one becomes read-only) |
| `DELETE /saves/{title}` | deletes the title's cloud saves (all profiles); does not reset the Free switch cooldown |
| `GET /saves/{title}/{profile}/versions` | history, newest first: `{version, size, sha256_ciphertext, committed_at, device_id, device_name}` |
| `POST /saves/{title}/{profile}/versions` | `{base_version, size, sha256_ciphertext (hex)}` -> `201 {version, upload_id, upload_url, upload_method: "PUT", upload_headers, expires_at}` |
| `POST /saves/{title}/{profile}/versions/{v}/commit` | optional `{upload_id}` -> the new head |
| `GET /saves/{title}/{profile}/versions/{v}` | the version + `download_url` (presigned GET, 15 min) |

Upload: `PUT upload_url` with exactly the `upload_headers` (`content-type: application/vnd.napple.save`,
`x-amz-checksum-sha256`, and the `content-length` the HTTP client sets from the body). The URL signs
Content-Length, Content-Type and the SHA-256 checksum, so S3 itself rejects any other body. Commit then checks
the object exists with the declared size.

Status codes: `409 version_conflict` with `head` when `base_version` is not the current head (at create or at
commit, whichever loses the race); `402 plan_limit` with `reason` (`plan_limit`, `switch_cooldown`, `quota`)
and `upgrade_url`; `507 quota_exceeded` on Plus; `413 too_large` above 64 MiB; `410 upload_expired` for a
pending upload older than a day; `400 upload_missing` / `upload_mismatch` at commit. Committing prunes the
oldest versions beyond the plan's count (10 Free, 20 Plus). A device's new upload for the same save replaces
its own unfinished one.

### Plans and billing

- **Free**: 1 synced title (the first title uploaded; `POST /saves/titles` switches once every 30 days), 10 versions, 256 MiB.
- **Napple Plus**: unlimited titles, 20 versions, 5 GiB. Prices are Stripe Price ids in config.
- Accounts, passkeys, devices, profiles and the online identity are free on both.

| Endpoint | |
|---|---|
| `GET /billing` | HTML plans page (`upgrade_url` points here); it sends the user to the app |
| `POST /billing/checkout` (auth) | `{interval: "monthly" \| "yearly"}` -> `{url}`: a Stripe Checkout session; the app opens it in the browser |
| `POST /billing/portal` (auth) | -> `{url}`: the Stripe Customer Portal (cancel, change card, switch interval) |
| `POST /billing/webhook` | Stripe events (configure the endpoint for `checkout.session.completed`, `customer.subscription.created/updated/deleted`, `customer.deleted`) |
| `GET /billing/success`, `/billing/cancel` | "return to Napple" pages |

Without the four Stripe variables every `/billing/*` answers 503 and every account is on Free. Statuses
`active`, `trialing` and `past_due` grant Plus (past_due while Stripe retries the card); anything else is Free.
Webhook deliveries are verified (`Stripe-Signature`, HMAC-SHA256 over `"<t>.<body>"`, 5-minute tolerance,
constant-time compare, several `v1` accepted for secret rotation) and idempotent (event ids are recorded in
the same SQLite transaction as their effect; a redelivery answers `{duplicate: true}`). Events older than the
last applied one for the same subscription are ignored (Stripe does not order deliveries).

## Security model

- **No passwords.** Sign-in is an emailed one-time code / link (10 min, single use, 5 attempts per code, at most
  20 guesses per address per hour, codes stored as HMAC, link tokens as SHA-256) or a passkey (user verification
  required, origin + RP ID + challenge + counter checked, ES256/EdDSA, pure-Rust verifier in `src/webauthn.rs`).
- **Tokens.** 15-minute Ed25519 JWTs, 30-day rotated refresh tokens stored hashed, reuse detection, per-device
  session epochs so logout and revoke take effect at once on this server.
- **End-to-end saves.** The server never sees the account save key, the recovery code, or save plaintext. It
  stores public keys, wrapped keys, the recovery blob, and ciphertext with its size and hash.
- **Abuse limits.** Per-IP limit on every auth endpoint (30/min), per-email limits on start (5/15 min) and finish;
  64 KiB JSON bodies, 64 MiB per save version, plan quotas (256 MiB / 5 GiB), only `application/vnd.napple.save` uploads.
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
| Stripe webhook forgery or replay | HMAC signature with timestamp tolerance; event-id idempotency; out-of-order guard | a leaked webhook secret lets an attacker grant Plus to accounts (billing only, no data access) |
| Using the bucket as free file hosting | per-version max size, quotas, content type pinned in the signed URL, short URL lifetimes | ciphertext cannot be inspected, by design |

## Data retention and deletion

- **Account deletion** (`DELETE /me` with the email typed as confirmation): deletes the Stripe customer (which
  cancels the subscription), queues every save object for deletion and deletes it right away, then removes the
  account and, through foreign keys, all devices, tokens, passkeys, profiles, titles and save metadata. Signing
  up again with the same email creates a new account. Objects that S3 could not delete at that moment stay in
  the deletion queue (object keys only, no personal data) and are retried every hour.
- **Downgrade or expiry** (subscription canceled or unpaid): the most recently saved titles up to the Free limit
  stay synced; the others become **read-only** (always downloadable, never uploadable) for 90 days. Seven days
  before the end an email warning goes out; the saves are deleted only after that warning was sent (at
  `max(read-only + 90 days, warning + 7 days)`). Upgrading again within the window makes them active again.
  The same applies to the title the Free plan switched away from.
- **Hourly retention job**: pending uploads older than a day (and their objects), expired codes, ceremonies and
  refresh tokens, Stripe event ids older than 90 days, read-only titles past their window, the deletion queue.
- **Versions**: the plan's newest N per (title, profile); older ones are deleted on the next commit.
- **S3 bucket** (napple-infra): SSE-S3, block public access, versioning with "delete noncurrent versions after 7
  days" and "abort incomplete multipart uploads after 1 day". Deleted saves are therefore gone from S3 within 7 days.
- **Stripe** keeps its own billing records as required by law; the account server keeps only the customer and
  subscription ids, status, interval and renewal date.
- **Logs** contain account and device ids, never emails or secrets.

## Deployment notes (for napple-infra)

- Image: `cargo build --release --locked -p napple-account --bin napple-account`; no system libraries beyond libc
  (rustls + ring, bundled SQLite). Read-only container, uid 10001, writes only under `NAPPLE_ACCOUNT_DATA`.
- Caddy: `handle /account/* { reverse_proxy account:8082 }` **without** stripping the prefix, ahead of the catch-all
  that goes to napple-online; set `NAPPLE_ACCOUNT_TRUST_PROXY=1`.
- Env: `NAPPLE_ACCOUNT_ENV=production`, `NAPPLE_ACCOUNT_MAILER=ses`, `NAPPLE_ACCOUNT_S3_BUCKET=napple-saves-<aws-account-id>`,
  `AWS_REGION`, and the four Stripe secrets from the secret store. Instance role: `s3:PutObject`, `s3:GetObject` (also
  authorizes HEAD) and `s3:DeleteObject` on `arn:aws:s3:::napple-saves-*/saves/*` and `ses:SendEmail` from `no-reply@napple.lat`.
  IMDSv2 hop limit 2 for containers.
- Stripe webhook endpoint `https://pretend.napple.lat/account/v1/billing/webhook`.
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

The scripted version is `crates/napple-account/scripts/walkthrough.sh` (all of the above plus the 409/402/503
cases, the download check and account deletion; `BIN=<path to napple-account>`, runs on port 18082 in a temp
dir and cleans up). Its 2026-11-02 output is in `docs/archive/changes/2026-11-02-claude-napple-account-2026-11-02-r8.md`.
