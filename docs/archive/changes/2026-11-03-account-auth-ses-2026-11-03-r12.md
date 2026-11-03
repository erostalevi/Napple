# 2026-11-03 account-auth-ses (r12): email + password accounts, SES/SMTP mail

napple-account sign-in is now email + password with emailed 6-digit codes. Passkeys and magic links are removed.

- Endpoints under `/account/v1/auth/`: `signup`, `verify`, `login`, `password/reset/start`, `password/reset/finish` (plus the existing `refresh`, `logout`).
- Argon2id (m=64 MiB, t=3, p=1), common-password list, single-use 10-min codes (5 attempts), 5 code mails/hour/address and 20/hour/IP, 10 failed logins/15 min -> 15 min lock, generic errors, terms + 13+ stored with version and time.
- Mailer providers `dev`, `ses` (SES v2, configuration set, multipart text + HTML from `templates/`), `smtp` (own minimal client, plain/STARTTLS/implicit TLS). Undeliverable addresses are recorded in `mail_suppressions` and never mailed again.
- Removed: `passkey.rs`, `webauthn.rs`, `/auth/email/*`, `/passkeys`, the link landing page, WebAuthn/link config.
- Env: `NAPPLE_MAIL_PROVIDER`, `NAPPLE_MAIL_FROM`, `NAPPLE_SES_REGION`, `NAPPLE_SES_CONFIG_SET`, `NAPPLE_SMTP_URL`, `NAPPLE_ACCOUNT_BIND`, `NAPPLE_ACCOUNT_DATA_DIR` (old names still accepted).

Verified: `cargo test -p napple-account` green (unit, auth, mail incl. fake SMTP server and SES request shape), scripted curl walkthrough, one real SES send accepted for talevineto@gmail.com (identity verified). Not verified: SMTP over TLS against a real relay; inbox rendering of the HTML.
Files: `crates/napple-account/{src,templates,tests,scripts}`, `docs/guides/napple-account.md`.
