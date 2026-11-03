# 2026-11-03 account-profiles (r12): profile fields, unique nicknames, sanitized photos, visibility tiers

New crate `napple-visibility` (pure `can_view(viewer, target, kind)` with the tier table; shared with napple-social).
`napple-account` gains `profiles.rs` (bio, links, icon 0..47, tier, Luhn friend code `NP-` + 12 digits, up to 8
profiles, idempotent migration of existing rows), `nicknames.rs` (3-20 `[A-Za-z0-9_.-]`, case-insensitive global
uniqueness, reserved names), `photos.rs` (pipeline: magic-byte type, <= 5 MiB, <= 8192 px, full decode, orientation,
centre crop, 256x256, JPEG q85 from pixels so all metadata is gone; `PhotoStore` with filesystem and private S3
backends) and `/internal/profiles*` lookups guarded by `NAPPLE_INTERNAL_TOKEN`. The old profile handlers in
`account.rs` are now unused (routes point at `profiles.rs`); `DELETE /me` also removes photo objects.

Verified: `cargo test -p napple-account -p napple-visibility` green (hostile-input tests: bombs, polyglots, truncated,
EXIF/GPS bytes, SVG/GIF/HEIC, zero size, oversize). Not run against real S3 (mock outbound only).
