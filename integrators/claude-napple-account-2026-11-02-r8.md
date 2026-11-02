---
branch: claude/napple-account-2026-11-02-r8
orchestration: napple-account
base: bc9a902
head: a49d0dd (code + docs; the commit adding this file sits on top)
env: mac
date: 2026-11-02
areas: [crates/napple-account, docs/guides]
status: ready-with-caveats
---

# claude/napple-account-2026-11-02-r8: the Napple Account server (identity, passkeys, E2E key exchange, cloud-save metadata, plans + Stripe)

## Summary
New workspace crate `crates/napple-account` (lib + binary `napple-account`, axum 0.8 + tokio, SQLite WAL via bundled
rusqlite) implementing `orchestrations/ACCOUNT-CONTRACT.md` server side: email magic link + 6-digit code, passkeys
(WebAuthn), devices with X25519 public keys, Ed25519 JWT access tokens + JWKS, rotated hashed refresh tokens with
reuse detection, the E2E save-key exchange (request/grant with a pinned `key_id`, recovery blob), up to 8 player
profiles, cloud-save version metadata (create/conflict 409/commit/list/prune) with SigV4-presigned S3 URLs and a
filesystem dev backend, quotas, the Free/Plus plans (1 title switchable every 30 days, 402 `plan_limit`, read-only 90
days after downgrade, warning email then deletion), Stripe Checkout/Portal/webhooks (signature + idempotency +
ordering), and account deletion. Everything is tested offline (41 tests). Guide: `docs/guides/napple-account.md`.
**Not done from the assignment:** webauthn-rs and aws-sdk-s3/sesv2 were **not** used (decision below); nothing ran
against real AWS, SES, Stripe or an Apple passkey; no Linux build was possible here.

## Files
- New: `crates/napple-account/` (`Cargo.toml`, `src/*.rs` 20 modules, `tests/{auth,keys,saves,billing,s3}.rs`,
  `tests/common/mod.rs`, `tests/fixtures/stripe/*.json` + README, `scripts/walkthrough.sh`).
- New: `docs/guides/napple-account.md`, `docs/archive/changes/2026-11-02-claude-napple-account-2026-11-02-r8.md`, this file.
- Shared, append-only: `Cargo.toml` (one member appended: `"crates/napple-account"`), `Cargo.lock` (169 packages
  added, **no existing package version changed**, checked by diffing the (name, version) sets).

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Crate builds, clippy clean, rustfmt clean | tested | `cargo build -p napple-account`, `cargo clippy -p napple-account --all-targets` (0 warnings), `cargo fmt -p napple-account --check` |
| A1 email start/finish (dev mailer) | tested | `tests/auth.rs`: single use, link token, case-insensitive email, 5-attempt lock, 10-min expiry |
| A1 passkey register/login with a test authenticator | tested | `tests/auth.rs::passkey_register_and_login` + `webauthn` unit tests (soft P-256 authenticator in `webauthn::soft`, not webauthn-rs; see Findings) |
| A1 refresh rotation + reuse detection | tested | `refresh_rotates_and_reuse_ends_the_device_session` |
| A1 device revoke | tested | `logout_and_device_revoke` |
| A1 key request/grant flow | tested | `tests/keys.rs` with real HPKE (hpke 0.12, X25519/HKDF-SHA256/AES-256-GCM) wraps and unwraps |
| A1 recovery blob | tested | `recovery_blob_round_trip` |
| A1 profiles | tested | `profiles_crud_limit_and_token_claims` |
| A1 save version create / 409 / commit / list / prune-to-20 | tested | `version_create_conflict_commit_list_download`, `prune_keeps_the_plan_version_count`, `default_prune_is_twenty_on_plus` |
| A1 quota | tested | `quota_returns_402_on_free` |
| A1 presign against a mock S3 endpoint | tested | `tests/s3.rs`: loopback mock checks SigV4 query signature + expiry, signed length/type/checksum, body checksum, header-signed HEAD/DELETE; SigV4 matches the two AWS doc vectors (`aws::tests`) |
| A1 account deletion | tested | `account_deletion_removes_everything` (every table empty, every blob gone), S3 variant in `tests/s3.rs` |
| A1 plans: Free 1 title, switch once/30 days, Plus unlimited, 402 | tested | `free_plan_one_title_switch_every_30_days`, `past_due_keeps_plus_…` |
| A1 Stripe webhook signature + idempotency with recorded test events | tested | `webhook_signature_and_idempotency` with `tests/fixtures/stripe/*.json` (authored in Stripe's documented shape, signed in-test; not captured from a live account) |
| A1 downgrade keeps read-only 90 days | tested | `past_due_keeps_plus_then_downgrade_is_read_only_then_deleted_after_warning` (warning at day 83, deletion at day 90) |
| A1 billing 503 without Stripe config | tested | `billing_is_503_without_stripe_config` |
| A2 local run: binary + sqlite in a temp dir + filesystem backend, curl walkthrough | verified | `BIN=<target>/release/napple-account crates/napple-account/scripts/walkthrough.sh` exit 0; output in the change note |
| A3 rate limits on auth endpoints | tested | `start_is_rate_limited_per_email_and_per_ip` (429 + Retry-After) |
| A3 constant-time compares | tested (by review) | `util::ct_eq` (subtle) for codes, link tokens, webhook signatures, WebAuthn challenges, dev-blob signatures |
| A3 codes single-use, refresh tokens hashed | tested | auth tests; schema stores `code_mac`, `token_hash`, refresh `hash` only |
| A3 CORS limited to napple.lat origins | tested | `cors_only_for_napple_origins` |
| A3 no secrets in logs | proposed (by review) | `log.rs` callers log ids only; outbound errors redact query strings; dev mailer prints codes by design and is refused in production |
| A3 never game content: max size + content type on uploads | tested | 413 above 64 MiB, `application/vnd.napple.save` enforced (signed header on S3, checked by the dev store) |
| Release binary dependencies | verified | `otool -L`: only `/usr/lib/libSystem.B.dylib` and `/usr/lib/libiconv.2.dylib` (no OpenSSL) |
| Linux build (infra wants Linux arm64/amd64) | untested | no Linux target/toolchain on this Mac; no macOS-only APIs used (`tokio::signal::unix` only) |

## Unbuilt / untested list (the integrator checks these first)
Nothing is unbuilt on macOS. Never run against the real services (code paths exercised only by mocks), all in commit 1438002:
- `mailer::SesMailer` (SES v2 SendEmail, SigV4 service `ses`) — never sent a real email.
- `blob::S3BlobStore` against real AWS S3 (virtual-hosted URLs `https://<bucket>.s3.<region>.amazonaws.com`); only path-style against the mock.
- `aws::CredentialProvider` IMDSv2 path — never run on EC2.
- `billing::{checkout, portal, cancel_everything}` against the real Stripe API; webhook handling against real Stripe deliveries.
- WebAuthn against a real platform authenticator (Apple passkeys via ASAuthorization, Chrome/Safari): only the soft authenticator.
- Linux build of the whole crate (ring and bundled SQLite compile C: needs a C compiler in the build image).

## Findings
- **Decision: no webauthn-rs / aws-sdk.** webauthn-rs needs OpenSSL, and the AWS SDK pulls aws-lc-sys (C + CMake) and a large tree. napple-infra's
  server contract asks for "no system libraries beyond libc" and a small read-only container. So: own WebAuthn RP verifier
  (`src/webauthn.rs`, about 300 lines, ES256 via p256, EdDSA via ed25519-dalek, CBOR via ciborium; attestation statement not
  verified because we request `none`), own SigV4 (`src/aws.rs`, checked against AWS's published vectors), HTTP via reqwest + rustls/ring.
  If the user wants webauthn-rs anyway, `passkey.rs` is the only caller of `webauthn::verify_*`; swapping is local.
- **Decision: S3 bucket name.** The contract's `napple-saves-<account-id>` is read as the **AWS** account id (one bucket); per-user buckets
  would hit the S3 bucket limit. Object keys: `saves/<account_id>/<TITLE>/<profile_id>/<upload_id>`.
- **Decision: `upgrade_url`** = `<public base>/billing` (HTML plans page). A browser has no bearer token, so the page sends the user to the
  app (`napple://account/upgrade`), and the app calls `POST /billing/checkout` and opens the returned Stripe URL.
- **Decision: Free switching.** The first title uploaded is enabled automatically; any later enable (explicit switch, or a new title after
  deleting the old one) is a switch subject to the 30-day cooldown. The switched-away title is read-only for 90 days like a downgrade.
- **Decision: downgrade order.** The most recently *saved* titles stay active; past_due keeps Plus (Stripe is still retrying).
- **Decision: account deletion** deletes the Stripe customer (cancels its subscriptions). If Stripe is configured but unreachable, deletion
  fails with 502 instead of deleting data while the user keeps getting billed.
- **Decision: first profile** is created with the account (nickname = email local part) so the online identity exists from sign-in.
- **Decision: added endpoints beyond the contract**: `GET/DELETE /passkeys`, `GET/POST /saves/titles`, `DELETE /saves/{title}`,
  `POST /billing/checkout|portal`, `GET /billing`, `/billing/success|cancel`, `/auth/email/link`, `/dev-blobs/*` (dev only).
- **Decision: response details** (unix-second times, base64url, `key_state`, `upload_headers`, error shapes): written down for the sibling
  sessions in the shared notes folder, `napple-account-api-notes.md`, and in the guide.
- Refresh races: strict reuse detection means a client that retries a refresh with the old token is signed out (device and key kept). Documented.
- Email codes are HMAC-ed with a per-process key: a restart voids codes still in flight (users ask for a new code). Deliberate.
- Rate limits are in memory (one instance behind one proxy). Several instances would need a shared store.
- Outside my area (not fixed): `crates/napple-online` (r7 branch) issues its own HS256 tokens; it still has to add JWKS verification of these
  EdDSA tokens and map `prf` profile ids to PIDs.

## How to integrate
- No dependencies on other branches. Merge any time; independent of cloud-saves-client (that branch only needs this API).
- Conflict hotspots: workspace `Cargo.toml` `members` (napple-online and napple-room append there too): keep every member. `Cargo.lock`:
  on conflict take either side and run `cargo generate-lockfile --offline` or plain `cargo build`; nothing here pins versions shared with
  other crates except through normal semver.
- Build and test:
  ```bash
  cargo test -p napple-account
  ```
  Expect `41 passed` across 6 test binaries (16 unit, 9 auth, 5 billing, 2 keys, 1 s3, 8 saves). First build compiles ring + SQLite (about 1 minute).
  ```bash
  cargo build --release -p napple-account --bin napple-account && BIN=target/release/napple-account crates/napple-account/scripts/walkthrough.sh
  ```
  Expect exit 0, `upload HTTP 200`, `409`, the 402 JSON, `download matches upload: 1`, `503`, `204`, `blobs left: 0`.
- No Linux-only calls. Everything is portable Rust on purpose (the server deploys to Linux).

## How to test
1. `cargo test -p napple-account` -> 41 passed, 0 failed. Failure looks like a named test panicking with the response JSON.
2. Walkthrough above (needs port 18082 free). The server log must show `listening on …`, `account created <uuid>`, `account deleted <uuid>`
   and no emails or tokens (apart from the dev mailer block, which is the dev mailer's job).
3. Regression guard: `cargo build` of the whole workspace still works (only a member was appended); no other crate changed.
4. Needs a human / deployment (napple-infra): real SES sender verified for `napple.lat`, an S3 bucket with the lifecycle rules in the guide,
   Stripe test-mode keys + a webhook endpoint, and an Apple passkey registration from the apps (associated domain `webcredentials:napple.lat`).

## Fix policy hints
- Safe to fix directly: dependency bumps, clippy/rustfmt drift, Linux build details (C compiler in the image), env names to match
  `infra/` (rename in `config.rs` and the guide's table).
- Needs a decision: switching to webauthn-rs or the AWS SDK (adds OpenSSL / aws-lc); changing plan semantics (switch rule, downgrade
  order, past_due handling); allowing a grace window for refresh-token reuse.

## Needs from other areas
- **napple-infra**: an `account` service (port 8082, data volume `/var/lib/napple/account`), Caddy `handle /account/* { reverse_proxy account:8082 }`
  ahead of the napple-online catch-all (no prefix strip), env/secrets and IAM as in the guide's "Deployment notes", IMDSv2 hop limit 2,
  the `napple-saves-<aws-account-id>` bucket with SSE-S3, block public access, versioning + noncurrent expiry 7 days, abort incomplete uploads 1 day.
- **napple-online**: verify these tokens via the JWKS (`jwt::verify_with_jwks` is the reference) and derive the online identity from a `profile_id` in `prf`.
- **cloud-saves-client**: follow `napple-account-api-notes.md` in the shared notes folder (HPKE `info` string, `key_id`, upload headers, error shapes, serialized refresh).
- **Apps (Studio/iOS)**: associated domains `webcredentials:napple.lat` + `applinks:napple.lat`, the `napple://auth/email` and `napple://account/upgrade` URL handlers.
