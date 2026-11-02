# 2026-11-02 claude/napple-account-2026-11-02-r8: the Napple Account server

New crate `crates/napple-account` (binary `napple-account`), the server side of `orchestrations/ACCOUNT-CONTRACT.md`
(round 8). Guide: `docs/guides/napple-account.md` (API, reserved names, security and threat model, retention and
deletion, env, deployment).

## What changed
- Sign-in: email 6-digit code + magic link (10 min, single use, HMAC/SHA-256 stored, attempt and rate limits), passkeys
  (WebAuthn registration with `attestation: none` and discoverable login, ES256/EdDSA, UV required, counter check;
  own pure-Rust verifier, `src/webauthn.rs`), devices with X25519 public keys, revoke, logout.
- Tokens: Ed25519 JWT access tokens (15 min; `sub`, `did`, `sep`, `prf`, `aud: napple`), JWKS at
  `/account/v1/.well-known/jwks.json`, key file with rotation; 30-day refresh tokens stored hashed, rotated, reuse detection.
- E2E save key: `/keys/requests`, `/keys/grant` (holder-only for other devices, optional pinned `key_id`), `/keys/mine`,
  `/keys/recovery` (the client's record, stored verbatim). The server never sees keys or plaintext.
- Profiles (≤ 8, the first created with the account), `/me` (static `plan {name: "free", features: {}}` + `storage`),
  account deletion (every object and row).
- Saves: per (title, profile) linear versions, `base_version` conflicts (409 with head) at create and commit, presigned
  S3 PUT/GET (SigV4; the PUT signs length, `application/octet-stream` and the SHA-256 checksum), commit checks the object,
  prune to `NAPPLE_SAVE_VERSIONS` (20), `NAPPLE_SAVE_QUOTA_BYTES` (10 GiB) and 64 MiB per version -> 413 `{reason}`.
  A filesystem dev backend serves its own signed URLs.
- Reserved names (`src/names.rs`): "napple" anywhere refused after look-alike normalisation (NFKD, marks dropped,
  confusables, leetspeak) except exact `napple` with grant `owner_napple`; `nap-`/`nap_`/`nap `/`nap.` prefix needs
  `reserved_prefix`; invisible characters refused; 422 `{reason: "reserved_name"}`; re-checked on rename.
- Grants and admins: `grants` table, `GET/POST/DELETE /admin/grants` (admins = `NAPPLE_ADMIN_EMAILS` + `admin` grant),
  `napple-account admin grant|revoke|list` CLI.
- Env-only config, `/healthz` and `/account/v1/healthz`, stdout logs without secrets or emails, SIGTERM shutdown,
  `rotate-jwt-key`, `retention` and `admin` subcommands. Workspace `Cargo.toml`: one member appended.
- Follow-up pass (contract change, same day): the first hand-off also had Free/Plus plans and Stripe billing; they
  were removed completely (code, tests, fixtures, schema) when the contract dropped paid plans for this round.

## Verified vs not
- tested: `cargo test -p napple-account` 43/43 on macOS arm64 (21 unit + 22 integration; offline: dev mailer, soft
  authenticator, real HPKE wraps, a local mock S3 on 127.0.0.1, moved clock, the CLI binary on a temp database).
  SigV4 matches the AWS documentation vectors (presigned GET and header-signed GET).
- verified: the release binary runs locally (`crates/napple-account/scripts/walkthrough.sh`, output below).
- untested: Linux build (no Linux target here; nothing macOS-specific is used), real SES, real S3, IMDSv2, real
  platform authenticators (Apple passkeys). All of them go through the code paths the mocks exercise.

## Walkthrough output (2026-11-02, release binary, `scripts/walkthrough.sh`)
```
== healthz
ok napple-account
== jwks
{"keys":[{"alg":"EdDSA","crv":"Ed25519","kid":"…","kty":"OKP","use":"sig","x":"…"}]}
== email start
{"expires_in":600,"sent":true}
== email finish (code from the dev mailer on stdout)
{'account_id': '01a19bdc-0e9d-731e-95a5-da3111dbaed1', 'device_id': '01a19bdc-0e9d-731e-95a5-da33def9b9cb', 'key_state': 'bootstrap', 'new_account': True, 'expires_in': 900}
== me
walk@napple.test {'features': {}, 'name': 'free'} 10737418240 20
== save version 1 (title LAS, profile 01a19bdc-0e9d-731e-95a5-da323d11fdde)
upload HTTP 200
== commit
{"committed_at":1793616252,"created_at":1793616252,"device_id":"01a19bdc-0e9d-731e-95a5-da33def9b9cb","device_name":"Walk Mac","sha256_ciphertext":"aca936974898bbd65a51a4e8cc9d608e8d0ddfbdf075f044e9aa7feff5d7bfe6","size":28,"version":1}
== stale base -> 409
409
== second title syncs too (no plans) -> 201
201
== reserved name -> 422
{"error":"reserved_name","field":"display_name","message":"names cannot contain \"Napple\"","reason":"reserved_name"}
== list
{"saves":[{"device_id":"01a19bdc-0e9d-731e-95a5-da33def9b9cb","device_name":"Walk Mac","latest_version":1,"profile_id":"01a19bdc-0e9d-731e-95a5-da323d11fdde","size":28,"title_id":"01006BB00C6F0000","updated_at":1793616252}]}
== download matches upload: 1
== delete account
204
== blobs left: 0
== server log (codes printed by the dev mailer only)
20261102T104412Z napple-account info listening on 127.0.0.1:18082 (public base http://127.0.0.1:18082/account/v1, blobs filesystem (dev))
20261102T104412Z napple-account info retention: Report { expired_uploads: 0, blobs_deleted: 0 }
20261102T104412Z napple-account info account created 01a19bdc-0e9d-731e-95a5-da3111dbaed1
20261102T104412Z napple-account info account deleted 01a19bdc-0e9d-731e-95a5-da3111dbaed1 (2 save objects queued)
```
