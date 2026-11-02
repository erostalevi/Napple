---
name: account-auth-ses
batch: 2026-11-03-r12
branch: claude/account-auth-ses-2026-11-03-r12
base: orchestrate/2026-11-03-r12
env: mac
model: claude-sonnet-5-5
depends_on: []
status: planned
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: none
launched_at: none
---

# account-auth-ses: email + password accounts with SES codes (napple-account auth side)

Read `orchestrations/ONLINE-CONTRACT.md` and `orchestrations/ENV-MAC.md` first; the contract is the source of truth for APIs, env names, tiers and the C API.

## Owned paths
`crates/napple-account/src/{signin,mailer,outbound,passkey,webauthn,account,retention}.rs` (rewrite/remove), `crates/napple-account/templates/`, new `crates/napple-account/src/{password,codes,ratelimit,terms}.rs`, tests `crates/napple-account/tests/{auth,mail}*.rs`, `docs/guides/napple-account.md` (auth sections), change note.

## Do not touch
Everything else: other r12 items edit the other paths in parallel (account auth/profiles, social, online server, client core, overlay, MK8D protocol, infra, legal), `orchestrations/`. Shared files (`Cargo.toml` members, `Cargo.lock`, `runtime/Makefile`, `docs/index.md`, `docs/archive/changes/README.md`): append-only and minimal. Use `cargo build -j2` / `cargo test -j2` (several sessions build at once on a 16 GB Mac).

## Work and acceptance
1. Implement the Accounts section of the contract: signup/verify/login/reset with Argon2id, common-password list, single-use 10-min codes, rate limits and lockout, generic errors, terms+13+ acceptance stored with version/timestamps; REMOVE passkeys and magic links (and their tests/docs).
2. Mailer: provider trait with `ses` (SES v2 SendEmail, config set, multipart HTML+text using the templates; SigV4 as the crate already does), `smtp` (a minimal client or lettre if acceptable) and `dev` (prints). Bounce/complaint suppression: handle SES suppression errors by marking the address undeliverable and not retrying.
3. Wire the existing HTML/text templates (placeholders in the file header) for confirmation and reset; unit-test rendering and HTML-escaping.
4. Tests: whole flows with the dev mailer (signup->verify->login->reset), lockout, rate limits, no enumeration, account deletion still works. `cargo test -p napple-account` green. Verify one real mail to talevineto@gmail.com ONLY IF that SES identity shows verified (`AWS_PROFILE=napple aws sesv2 get-email-identity --email-identity talevineto@gmail.com`); otherwise note it.
5. Hand off with the exact endpoints/env names implemented.

## Env notes
Shared title data read-only; game runs under `lockf -k /Users/neto/NappleWork/.run.lock` (<=120 s, private --save-root); local only, never push; never ask the user; never commit credentials or Nintendo material. Test-first, small commits.

## Result
