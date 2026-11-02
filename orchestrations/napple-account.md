---
name: napple-account
batch: 2026-11-02-r8
branch: claude/napple-account-2026-11-02-r8
base: orchestrate/2026-11-02-r8
env: mac
model: claude-opus-5-5
depends_on: []
status: planned
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: none
launched_at: none
---

# napple-account: the Napple Account server (email magic link + passkeys, devices, E2E key exchange, cloud-save metadata, plans + Stripe billing)

## Goal
Implement `orchestrations/ACCOUNT-CONTRACT.md` server side as a new Rust crate/binary `crates/napple-account` (axum or hyper; sqlite via rusqlite/sqlx;
webauthn-rs for passkeys; ed25519 JWT; aws-sdk-s3 presigning; SES via aws-sdk-sesv2 with a local "print the code to stdout" mailer for dev). Env-only
config, `/account/v1/healthz`, logs to stdout, Linux-buildable. napple-online (round 7, still running) will verify these JWTs via JWKS — do not edit its crate.

## Owned paths
`crates/napple-account/` (new; workspace `Cargo.toml` members append-only), `docs/guides/napple-account.md` (new: API, security model, threat model,
data retention/deletion — include account deletion that removes all saves), own change note.

## Do not touch
`crates/napple-online/`, `crates/napple-room/`, `runtime/`, `studio/`, `infra/` (napple-infra adds deployment), `orchestrations/`.

## Acceptance
1. `cargo test -p napple-account`: email start/finish (dev mailer), passkey register/login with webauthn-rs test authenticator, refresh rotation and reuse
   detection, device revoke, key request/grant flow, recovery blob, profiles, save version create/conflict(409)/commit/list/prune-to-20, quota, presign
   against a mock S3 endpoint, account deletion; plans: Free = 1 title (switch once per 30 days), Plus = unlimited; 402 on the limit; Stripe webhook signature
   verification + idempotency with recorded test events (no network); downgrade keeps read-only access 90 days; billing endpoints 503 without Stripe config.
2. Local run: binary starts with sqlite in a temp dir and a local S3-compatible mock (or a filesystem backend behind a trait for dev), curl walkthrough in the guide.
3. Security: rate limits on auth endpoints, constant-time compares, codes single-use, refresh tokens stored hashed, CORS limited to napple.lat origins,
   no secrets in logs. Never stores or accepts game content, keys or firmware (enforce max sizes and content-type on save uploads only).

## Env notes
Read `orchestrations/ENV-MAC.md` (local-only, never ask, never push). Never write real AWS credentials anywhere; tests use mocks.

## Result
