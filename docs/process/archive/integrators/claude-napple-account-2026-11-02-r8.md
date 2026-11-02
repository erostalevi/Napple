---
branch: claude/napple-account-2026-11-02-r8
orchestration: napple-account
base: bc9a902
head: see `git log -1 claude/napple-account-2026-11-02-r8` (the commit carrying this file)
env: mac
date: 2026-11-02
areas: [crates/napple-account, docs/guides]
status: integrated
---

# claude/napple-account-2026-11-02-r8: the Napple Account server (identity, passkeys, devices, JWT/JWKS, E2E key exchange, profiles with reserved names, cloud-save metadata)

## Summary
New workspace crate `crates/napple-account` (lib + binary `napple-account`, axum 0.8 + tokio, bundled SQLite in WAL
mode) implementing `orchestrations/ACCOUNT-CONTRACT.md` as of `orchestrate/2026-11-02-r8` (4b42cd4) on the server side:
- **Sign-in:** email magic link + 6-digit code, and passkeys (WebAuthn).
- **Devices and tokens:** devices with X25519 public keys; Ed25519 JWT access tokens + JWKS; rotated, hashed refresh
  tokens with reuse detection.
- **E2E save-key exchange:** request/grant, recovery record.
- **Profiles:** up to 8 player profiles, with reserved-name rules on display names and nicknames, and admin grants
  (API + CLI).
- **Cloud saves:** version metadata (create, 409 conflict, commit, list, prune to 20), SigV4-presigned S3 URLs and a
  filesystem dev backend, a config quota answering 413 `{reason:"quota"}`.
- **Account deletion.**
- **Billing:** none, per the contract change. The first hand-off (1438002/a49d0dd/a4529f0) also had Free/Plus plans and
  Stripe; that is removed completely (code, tests, fixtures, schema columns). `GET /me` returns the static
  `plan {name:"free", features:{}}`.

43 offline tests. Guide: `docs/guides/napple-account.md`.
**Not done:**
- No real AWS, SES or Apple passkey was used.
- No Linux build was possible here.
- webauthn-rs and the AWS SDK were deliberately not used (see Findings).

## Files
- New: `crates/napple-account/`:
  - `Cargo.toml`
  - `src/*.rs`, 21 modules
  - `tests/{auth,keys,names,saves,s3}.rs` and `tests/common/mod.rs`
  - `scripts/walkthrough.sh`
- New docs: `docs/guides/napple-account.md`, `docs/archive/changes/2026-11-02-claude-napple-account-2026-11-02-r8.md`, this file.
- Shared files, append-only:
  - `Cargo.toml`: one member appended, `"crates/napple-account"`.
  - `Cargo.lock`: packages added only. No existing package version changed, checked by diffing the (name, version) sets.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Crate builds, clippy clean, rustfmt clean | tested | `cargo build -p napple-account`, `cargo clippy -p napple-account --all-targets` (0 warnings), `cargo fmt -p napple-account --check` |
| Email start/finish (dev mailer) | tested | `tests/auth.rs`: single use, link token, case-insensitive email, 5-attempt lock, 10-min expiry, rate limits (429 + Retry-After) |
| Passkey register/login with a test authenticator | tested | `tests/auth.rs::passkey_register_and_login` + `webauthn` unit tests (software P-256 authenticator `webauthn::soft`) |
| Refresh rotation + reuse detection, logout, device revoke | tested | `refresh_rotates_and_reuse_ends_the_device_session`, `logout_and_device_revoke` |
| JWT + JWKS as napple-online will verify | tested | `jwks_verifies_access_tokens_like_napple_online_will` (`jwt::verify_with_jwks`) |
| Key request/grant, recovery | tested | `tests/keys.rs` with real HPKE (hpke 0.12) wraps/unwraps; `client_wire_shapes_without_key_id` (the cloud-saves client's grant and recovery record shapes) |
| Profiles (≤ 8, isolation between accounts, delete with saves) | tested | `profiles_crud_limit_and_token_claims` |
| Save versions create / 409 / commit / list / download / prune | tested | `version_create_conflict_commit_list_download`, `prune_keeps_the_plan_version_count` |
| Quota 413 `{reason:"quota"}`, 64 MiB 413 `{reason:"too_large"}` | tested | `quota_returns_413`, the size check in the version test |
| Presign against a mock S3 endpoint | tested | `tests/s3.rs` (loopback mock checks the SigV4 query signature and expiry, the signed length/type/checksum, the body checksum, header-signed HEAD/DELETE); SigV4 = AWS doc vectors (`aws::tests`) |
| Account deletion | tested | `account_deletion_removes_everything` (every table empty, every blob gone) + the S3 variant |
| Reserved names (every rule, look-alikes) | tested | `names::tests` (unit: napple anywhere incl. Cyrillic/Greek/fullwidth/math/accents/leet/spacing, exact `napple` only with `owner_napple`, `nap-` prefix variants incl. en dash and fullwidth, invisible characters, ordinary names such as Naples/napkin/Pineapple pass) + `tests/names.rs` (422 on display_name and nicknames, default name `Player`) |
| Admin grants API, re-check on rename, admin grant | tested | `admin_grants_unlock_names_and_rename_rechecks` |
| `napple-account admin` CLI | tested | `admin_cli_grants_in_the_database` (runs the built binary on a temp database) |
| Local run: binary + sqlite in a temp dir + filesystem backend, curl walkthrough | verified | `BIN=<target>/release/napple-account crates/napple-account/scripts/walkthrough.sh` exit 0; output in the change note |
| CORS limited to napple.lat origins | tested | `cors_only_for_napple_origins` |
| No secrets in logs | proposed (by review) | log callers pass ids only; outbound errors drop query strings; the dev mailer prints codes by design and production config refuses it |
| Release binary dependencies | verified | `otool -L`: only `/usr/lib/libSystem.B.dylib` and `/usr/lib/libiconv.2.dylib` (no OpenSSL) |
| Linux build | untested | no Linux target here; no macOS-only APIs used (`tokio::signal::unix` only) |

## Unbuilt / untested list (the integrator checks these first)
Nothing is unbuilt on macOS. These were never run against the real services; only mocks exercise their code paths:
- `mailer::SesMailer` (SES v2 SendEmail): no real email was ever sent.
- `blob::S3BlobStore` against real AWS. Only the mock was used, path-style; AWS itself is addressed virtual-hosted style.
- `aws::CredentialProvider` IMDSv2 path: never run on EC2.
- WebAuthn against a real platform authenticator (Apple passkeys via ASAuthorization, browsers).
- Linux build of the crate: ring and bundled SQLite compile C, so the build image needs a C compiler.

## Findings
- **Decision: no webauthn-rs, no aws-sdk.** webauthn-rs needs OpenSSL, and the AWS SDK pulls in aws-lc-sys (C + CMake)
  and a large dependency tree. napple-infra's server contract asks for no system libraries beyond libc. So the crate has:
  - its own WebAuthn RP verifier (`src/webauthn.rs`: ES256 via p256, EdDSA via ed25519-dalek, CBOR via ciborium; the
    attestation statement is not verified because we request `none`);
  - its own SigV4 signer (`src/aws.rs`, checked against AWS's published vectors);
  - HTTP via reqwest + rustls/ring.
  Swapping either back is local: `passkey.rs` is the only caller of `webauthn::verify_*`, and `blob.rs`/`mailer.rs` the
  only users of `aws.rs`.
- **Decision: bucket naming.** I read the contract's `napple-saves-<account-id>` as the AWS account id (one bucket).
  Object keys are `saves/<account_id>/<TITLE>/<profile_id>/<upload_id>`.
- **Decision: client wire alignment.** I read the cloud-saves-client branch (`crates/napple-cloud`) and its note
  `cloud-saves-client-wire.md`, and changed the server to accept what the client sends:
  - `key_id` is optional on `/keys/grant`;
  - the recovery record is an opaque JSON object, stored verbatim;
  - uploads are `application/octet-stream`;
  - `GET .../versions/{v}` has `url`, and history entries have `created_at`;
  - 409 `head` is an object whose `version` field the client reads.
- **Remaining client/server mismatch (integrator: align the client):** the presigned PUT signs
  `x-amz-checksum-sha256` and `content-length` along with the content type. The client's `put_bytes`
  (`crates/napple-cloud/src/api.rs`) sends only `Content-Type`, so real S3 refuses the upload. The dev store accepts it, because it checks the body's SHA-256 itself. The fix
  is client side: send every header in the create response's `upload_headers` verbatim (`x-amz-checksum-sha256` =
  base64 of the SHA-256 the client already computes). The alternative, dropping the checksum from the signature,
  loses the server-side integrity check and needs a decision.
- **Decision: names.**
  - Device and passkey names are only visible to the owner, so they are not name-checked.
  - "Exact `napple`" means the trimmed name equals `napple` ignoring ASCII case.
  - `Snapple` is refused, since it contains the word.
  - A new account whose email local part is reserved is named `Player`.
  - Grants include `admin`, so admins can be added without a config change.
- Refresh races: strict reuse detection signs out a client that retries a refresh with the old token. The device and
  its key are kept, because signing in again with the same device key reuses the device. Documented.
- Email codes are HMAC-ed with a per-process key, so a restart voids codes still in flight. Rate limits are in memory,
  so they hold for one instance only.
- Outside my area (not fixed): napple-online (r7 branch) issues its own HS256 tokens. It still has to verify these
  EdDSA tokens via the JWKS and map `prf` profile ids to PIDs.

## How to integrate
- No dependencies on other branches; merge any time.
- Conflict hotspots:
  - workspace `Cargo.toml` `members`: napple-online and napple-room append there too; keep every member.
  - `Cargo.lock`: on conflict take either side and run `cargo build` to regenerate.
- Build and test:
  ```bash
  cargo test -p napple-account
  ```
  Expect 43 passed: 21 unit, then integration 9 auth, 3 keys, 3 names, 1 s3, 6 saves. The first build compiles ring
  and SQLite (about 1 min). The names CLI test runs the `napple-account` binary that cargo builds for the tests.
  ```bash
  cargo build --release -p napple-account --bin napple-account && BIN=target/release/napple-account crates/napple-account/scripts/walkthrough.sh
  ```
  Expect exit 0 with: `upload HTTP 200`, `409`, `201` (second title), the 422 `reserved_name` JSON, `download matches upload: 1`, `204`, `blobs left: 0`.
- No Linux-only calls are used. Everything is portable Rust on purpose, because the server deploys to Linux.

## How to test
1. `cargo test -p napple-account`: 43 passed, 0 failed.
2. The walkthrough above (needs port 18082 free). The server log shows `listening on …`, `account created <uuid>` and
   `account deleted <uuid>`, with no emails or tokens outside the dev mailer block.
3. Regression guard: the whole workspace still builds. Only a member line was added; no other crate changed.
4. Needs a human or a deployment (napple-infra):
   - a verified SES sender for `napple.lat`;
   - the S3 bucket with the lifecycle rules in the guide;
   - `NAPPLE_ADMIN_EMAILS` set to the owner's email, then `napple-account admin grant <owner email> owner_napple`;
   - an Apple passkey registration from the apps (associated domain `webcredentials:napple.lat`).

## Fix policy hints
- Safe to fix directly: dependency bumps, clippy/rustfmt drift, Linux build details, env names to match `infra/`
  (rename in `config.rs` and the guide table), extra confusables in `names.rs` (add to the table and the tests).
- Needs a decision:
  - switching to webauthn-rs or the AWS SDK (adds OpenSSL / aws-lc);
  - the upload checksum header (client change vs. dropping it from the signature);
  - a grace window for refresh-token reuse;
  - re-introducing paid plans (the removed implementation is in commit 1438002 if useful).

## Needs from other areas
- **cloud-saves-client:** send the create response's `upload_headers` on the PUT (see Findings). Everything else in
  its wire note now matches.
- **napple-infra:**
  - an `account` service (port 8082, data volume `/var/lib/napple/account`);
  - Caddy `handle /account/* { reverse_proxy account:8082 }` ahead of the napple-online catch-all, with no prefix strip;
  - env and IAM as in the guide's "Deployment notes", including `NAPPLE_ADMIN_EMAILS`;
  - IMDSv2 hop limit 2;
  - the `napple-saves-<aws-account-id>` bucket: SSE-S3, block public access, versioning with noncurrent expiry 7 days,
    abort incomplete uploads after 1 day.
- **napple-online:** verify these tokens via the JWKS (`jwt::verify_with_jwks` is the reference), derive the online
  identity from a `profile_id` in `prf`, and apply `names::check_name` rules to any name it lets players choose.
- **napple-room:** room names shown to others should follow the same reserved-name rules.
- **Overlay Account tab (frontend):** show 422 `reserved_name` messages; associated domains `webcredentials:napple.lat`
  + `applinks:napple.lat`; a `napple://auth/email` handler.

## Integration result
- Date 2026-11-02, merged into main as 69edb14 (head 9167b95 as handed off: billing removed, reserved names, admin grants, client wire alignment).
  First of batch r8, after the r7 items. Conflicts: `Cargo.toml` members only (kept napple-room, napple-online and napple-account); `Cargo.lock`
  auto-merged and `cargo build --workspace --locked` passes. Outcome: **Clean**, plus one doc commit (guides index row for `napple-account.md`; round
  r8 section in the change archive with this change note; the unattended-rounds range and its anchors in `docs/index.md` and the process archive moved
  to r8; `tools/check_docs.py` reported both new docs as orphans before).
- No native ports (portable Rust on purpose; it deploys to Linux).
- `cargo test -p napple-account --locked`: 43 passed (21 unit; auth 9, keys 3, names 3, s3 1, saves 6). The workspace `cargo test --workspace --locked`
  ran green on a tree with this branch and cloud-saves-client.
- Walkthrough (`cargo build --release -p napple-account --bin napple-account`, `scripts/walkthrough.sh`): exit 0, `upload HTTP 200`, `409`, `201`,
  the 422 `reserved_name` JSON, `download matches upload: 1`, `204`, `blobs left: 0`; server log shows account created / deleted only.
- JWKS / profiles as napple-social consumes them: verified live when napple-social landed (see its integrator file): a real access token from this
  server is accepted by napple-social through `/account/v1/.well-known/jwks.json` and `/account/v1/profiles`; a garbage token gets 401.
- The cloud-saves-client upload-header mismatch (Findings) was fixed on the client side when cloud-saves-client landed (see its integrator file).
- Still unverified (as declared): SES, real S3 (virtual-hosted style), IMDSv2, real passkeys, Linux build. napple-online still issues its own HS256
  tokens (JWKS verification and `prf` → PID mapping are a follow-up).
