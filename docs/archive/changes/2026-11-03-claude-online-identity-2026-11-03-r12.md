# napple-online: account JWT logins, ghost alias, match log, social reporting (r12)

New modules in `crates/napple-online`: `identity.rs` (Ed25519 JWT verification via cached JWKS, owned-profile lookup, deterministic pid from profile_id), `ghost.rs` (tier-5 aliases), `matchlog.rs` (match log, 30-day retention, alias resolution, export), `reporting.rs` (encounters + stats to napple-social with the service credential, retrying outbox), `hub.rs` (glue: login, join/leave hooks, admin HTTP), `upstream.rs` (tiny http:// client). Small hooks in `server.rs`, `session.rs`, `main.rs`. Enabled by `NAPPLE_ACCOUNT_URL`; without it the legacy behaviour is untouched.

Tested (mock account/social, `cargo test -p napple-online`: 49 unit + 1 integration). Not run against the real napple-account/napple-social processes (they are other r12 items).

Docs: `docs/guides/napple-online.md` (new section).
