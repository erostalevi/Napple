# 2026-11-02 claude/napple-account-2026-11-02-r8: the Napple Account server

New crate `crates/napple-account` (binary `napple-account`), the server side of `orchestrations/ACCOUNT-CONTRACT.md`
(round 8). Guide: `docs/guides/napple-account.md` (API, security model, threat model, retention/deletion, env, deploy).

## What changed
- Sign-in: email 6-digit code + magic link (10 min, single use, HMAC/SHA-256 stored, attempt and rate limits), passkeys
  (WebAuthn registration with `attestation: none` and discoverable login, ES256/EdDSA, UV required, counter check;
  own pure-Rust verifier, `src/webauthn.rs`), devices with X25519 public keys, revoke, logout.
- Tokens: Ed25519 JWT access tokens (15 min; `sub`, `did`, `sep`, `prf`, `aud: napple`), JWKS at
  `/account/v1/.well-known/jwks.json`, key file with rotation; 30-day refresh tokens stored hashed, rotated, reuse detection.
- E2E save key: `/keys/requests`, `/keys/grant` (holder-only for other devices, `key_id` pinned), `/keys/mine`,
  `/keys/recovery` (opaque blob + KDF params). Server never sees keys or plaintext.
- Profiles (≤ 8, first one created with the account), `/me`, account deletion (Stripe customer deleted, every object deleted).
- Saves: per (title, profile) linear versions, `base_version` conflicts (409 with head) at create and commit, presigned
  S3 PUT/GET (SigV4; PUT pins length, content type and SHA-256 checksum), commit checks the object, prune to the plan's
  count, quota, 64 MiB max, `application/vnd.napple.save` only. Filesystem dev backend serves its own signed URLs.
- Plans: Free = 1 title (switch once per 30 days), 10 versions, 256 MiB; Plus = unlimited, 20, 5 GiB; 402 `plan_limit`
  with `upgrade_url`; downgrade/switch-away titles read-only 90 days, warning email, then deletion (hourly retention job).
- Stripe: Checkout + Customer Portal (form POST to the Stripe API, idempotency key), webhook with signature verification,
  event-id idempotency in the same transaction, out-of-order guard; 503 everywhere without Stripe config.
- Env-only config, `/healthz` and `/account/v1/healthz`, stdout logs without secrets or emails, SIGTERM shutdown,
  `rotate-jwt-key` and `retention` subcommands. Workspace `Cargo.toml`: one member appended.

## Verified vs not
- tested: `cargo test -p napple-account` 41/41 on macOS arm64 (16 unit + 25 integration; offline: dev mailer, soft
  authenticator, real HPKE wraps, local mock S3 on 127.0.0.1, mock Stripe API, signed webhook fixtures, moved clock).
  SigV4 matches the AWS documentation vectors (presigned GET and header-signed GET).
- verified: the release binary runs locally (`crates/napple-account/scripts/walkthrough.sh`, output below).
- untested: Linux build (no Linux target here; nothing macOS-specific is used), real SES, real S3, real Stripe, IMDSv2,
  real platform authenticators (Apple passkeys) — all behind the same code paths the mocks exercise.

## Walkthrough output (2026-11-02, release binary, `scripts/walkthrough.sh`)
```
== healthz
ok napple-account
== jwks
{"keys":[{"alg":"EdDSA","crv":"Ed25519","kid":"…","kty":"OKP","use":"sig","x":"…"}]}
== email start
{"expires_in":600,"sent":true}
== email finish (code from the dev mailer on stdout)
{'account_id': '01a19b40-960a-76fd-a5bb-eeacc68fcf67', 'device_id': '01a19b40-960a-76fd-a5bb-eeaef4dd0f80', 'key_state': 'bootstrap', 'new_account': True, 'expires_in': 900}
== me
walk@napple.test free 1 268435456
== save version 1 (title LAS, profile 01a19b40-960a-76fd-a5bb-eead814e2308)
upload HTTP 200
== commit
{"committed_at":1793606063,"device_id":"01a19b40-960a-76fd-a5bb-eeaef4dd0f80","device_name":"Walk Mac","sha256_ciphertext":"aca936974898bbd65a51a4e8cc9d608e8d0ddfbdf075f044e9aa7feff5d7bfe6","size":28,"version":1}
== stale base -> 409
409
== second title on Free -> 402
{"error":"plan_limit","message":"the Free plan syncs one title; switch titles or upgrade to Napple Plus","next_switch_at":1793606063,"reason":"plan_limit","synced_titles_limit":1,"upgrade_url":"http://127.0.0.1:18082/account/v1/billing"}
== list
{"saves":[{"delete_after":null,"device_id":"01a19b40-960a-76fd-a5bb-eeaef4dd0f80","device_name":"Walk Mac","latest_version":1,"profile_id":"01a19b40-960a-76fd-a5bb-eead814e2308","read_only":false,"size":28,"title_id":"01006BB00C6F0000","updated_at":1793606063}]}
== download matches upload: 1
== billing without Stripe -> 503
503
== delete account
204
== blobs left: 0
== server log (codes printed by the dev mailer only)
20261102T075423Z napple-account info listening on 127.0.0.1:18082 (public base http://127.0.0.1:18082/account/v1, billing off, blobs filesystem (dev))
20261102T075423Z napple-account info retention: Report { expired_uploads: 0, warned: 0, deleted_titles: 0, blobs_deleted: 0 }
20261102T075423Z napple-account info account created 01a19b40-960a-76fd-a5bb-eeacc68fcf67
20261102T075423Z napple-account info account deleted 01a19b40-960a-76fd-a5bb-eeacc68fcf67 (1 save objects queued)
```
