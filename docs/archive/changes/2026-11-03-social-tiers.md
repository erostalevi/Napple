# 2026-11-03 social-tiers (round 12)

napple-social gains the five visibility tiers and the features that depend on them: tier-aware search (nickname prefix, friend code), friend-request
rules including the shared-lobby exception, the filtered public profile view, per-profile stats, lobby views with ghost aliases, recent players fed by
`POST /encounters`, reports (also of ghosts by alias), and the profile snapshot (tier, photo) from napple-account with a short-TTL service lookup.
Block hides both ways everywhere; ghost friends see nothing. Schema version 2 (migrates in place).

Tested: `cargo test -p napple-social` (22 unit + 59 integration, 30 of them in `tests/tiers.rs` covering every tier x action cell). Not run against a real
napple-account (the mock account service in `tests/common` stands in; the service-credential directory endpoint is a proposal, see the hand-off notes).

Files: `crates/napple-social/` (new `src/visibility.rs`, `src/social.rs`, `tests/tiers.rs`), `docs/guides/napple-social.md`.
