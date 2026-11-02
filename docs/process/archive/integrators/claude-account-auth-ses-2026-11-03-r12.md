---
branch: claude/account-auth-ses-2026-11-03-r12
orchestration: account-auth-ses
base: ac6692c
head: 05c0462
env: mac
date: 2026-11-03
areas: [napple-account auth, mail]
status: integrated
---

# account-auth-ses: email + password accounts with SES codes (napple-account auth side)

## Summary
Replaced magic-link/passkey sign-in with signup -> emailed code -> verify -> login -> password reset (Argon2id, common-password list, single-use 10-min codes with 5 attempts, 5 code mails/h/address and 20/h/IP, 10 failed logins/15 min -> 15 min lock, no enumeration, terms+13+ stored with version/time). Mailer providers dev/ses/smtp with the multipart templates; SES suppression errors and SMTP recipient 5xx mark the address undeliverable (table `mail_suppressions`). Passkeys/magic links, their routes, tests and docs are gone. Not done: an SNS bounce/complaint webhook (SES account-level suppression handles it; the server learns of suppression only from send errors); SMTP against a real TLS relay.

## Files
`crates/napple-account`: new `src/{password,codes,ratelimit,terms}.rs`, `src/common_passwords.txt`, `tests/mail.rs`; rewritten `signin.rs`, `mailer.rs`; removed `passkey.rs`, `webauthn.rs`; edited `app.rs` (routes), `config.rs`, `db.rs`, `lib.rs`, `pages.rs` (link page removed), `retention.rs`, `account.rs`, `Cargo.toml` (+argon2, tokio-rustls, webpki-roots; -ciborium, -p256 dep; all already in Cargo.lock). Tests: `tests/auth.rs` rewritten, `tests/common/mod.rs` (helper now signs up/logs in with PASSWORD), `tests/names.rs` (one assertion: default name from email replaced by reserved-nickname signup refusal). Docs: `docs/guides/napple-account.md`, guides README row, change note.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Whole flows (signup->verify->login->reset), lockout, rate limits, no enumeration, deletion, suppression, retention | tested | `cargo test -j2 -p napple-account`: all green (25 lib, 16 auth, 3 mail, others) |
| Templates render + HTML escaping | tested | `mailer::tests` |
| SES request (SigV4, config set, text+HTML) and suppression mapping | tested | `tests/mail.rs` with MockOutbound |
| SMTP client | tested | `tests/mail.rs` against a fake plain SMTP server; TLS/STARTTLS paths untested |
| Real SES send | verified (accepted) | identity talevineto@gmail.com VerificationStatus SUCCESS; server with NAPPLE_MAIL_PROVIDER=ses answered 202 (a send error would be 503). Inbox not inspected |
| curl walkthrough | verified | `scripts/walkthrough.sh` (updated) |
| Argon2id production parameters | tested | `password::tests` checks the PHC string m=65536,t=3,p=1 |

## Unbuilt / untested list
SMTP STARTTLS and implicit-TLS code paths in `mailer.rs` (compiled, never connected to a TLS server).

## Findings
- Decisions: nickname on signup is validated (3-20, [A-Za-z0-9_.-], reserved names) and unique vs profiles and pending signups; first profile is created at verify with a plain INSERT into `profiles(id, account_id, nickname, icon_id, created_at, updated_at)` (`signin::verify`): if account-profiles adds NOT NULL columns (tier, friend_code) this INSERT must call their creator.
- Code HMAC key is per process (as before): a restart voids codes in flight.
- Login lockout is keyed by the typed address (known or not), stored in `login_failures`.
- Unverified signups are deleted by retention after 24 h. Legacy accounts with a device are migrated to verified; they have no password and use reset.
- Old env names (`NAPPLE_ACCOUNT_MAILER`, `_HTTP`, `_DATA`, `_MAIL_FROM`, `_SES_REGION`) are still accepted.
- New schema is applied by `codes::migrate` (called from `db::init`): ALTER TABLE accounts ADD COLUMN ..., drops passkey tables.

## How to integrate
- Conflict hotspots: `db.rs`, `app.rs` (router), `config.rs`, `tests/common/mod.rs` and `lib.rs` with account-profiles; keep both sides. `Cargo.lock` changes only the crate's dependency list.
- Build/test: `cargo test -j2 -p napple-account` (needs the rust toolchain; on this Mac `~/.rustup/toolchains/stable-aarch64-apple-darwin/bin`).
- **Breaks `crates/napple-cloud/src/session.rs`** (client-core owns it): it still calls `/auth/email/start|finish`; must move to `/auth/signup|verify|login` (and `docs/guides/cloud-saves.md`). `orchestrations/ACCOUNT-CONTRACT.md` is outdated (not edited).

## How to test
1. `cargo test -j2 -p napple-account` -> all ok.
2. `BIN=target/debug/napple-account bash crates/napple-account/scripts/walkthrough.sh` -> ends with account deleted, blobs left 0.
3. Real mail: `NAPPLE_MAIL_PROVIDER=ses` with AWS credentials in env (`aws configure export-credentials --profile napple --format env`), POST /auth/signup to talevineto@gmail.com -> 202 and an email arrives.

## Fix policy hints
Safe: dependency/feature typos, route names. Design decision: how profile creation at verify should call the profiles module.

## Needs from other areas
client-core: update napple-cloud/overlay API to `signup/verify/login/reset`. infra: set `NAPPLE_MAIL_PROVIDER=ses`, `NAPPLE_SES_CONFIG_SET=napple-transactional`; instance role needs ses:SendEmail on the config set. account-profiles: see profile INSERT note.

## Integration result
- Date: 2026-11-03 (batch r12, rolling). Merged at a1482a6. Outcome: Small fix + mechanical conflicts (Cargo.toml, account.rs, app.rs, db.rs, lib.rs, changes index: kept both sides, passkey module dropped).
- integrate: fixes: signin::verify creates the first profile via nicknames::validate + profiles::insert (was raw INSERT), with test signup_yields_a_valid_unique_first_profile_nickname
- Native ports: none (Rust only). Tests: cargo test --workspace: 497 passed, 0 failed, 2 ignored; check_docs baseline only archive orphans; gen_support OK
- Corrections to instructions: cargo needs PATH=/opt/homebrew/opt/rustup/bin; used a shared CARGO_TARGET_DIR.
