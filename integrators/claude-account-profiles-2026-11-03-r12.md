---
branch: claude/account-profiles-2026-11-03-r12
orchestration: account-profiles
base: ac6692c
head: HEAD_PLACEHOLDER
env: mac
date: 2026-11-03
areas: [napple-account, napple-visibility]
status: ready
---

# claude/account-profiles-2026-11-03-r12: profiles, unique nicknames, sanitized photos, visibility tiers

## Summary
Implements the contract's Profiles section in napple-account and a new `napple-visibility` crate (pure `can_view`, tier table,
7 unit tests). Profiles: bio, links (https-only web), icon 0..47, tier 1..5, Luhn friend code, 8/account, case-insensitive
unique nicknames, reserved names. Photo pipeline exactly as specified, fs + private-S3 stores behind `PhotoStore`.
Not done: real-S3 run (mock only); napple-social side (it must call `can_view` and the internal routes).

## Files
New: crates/napple-visibility/*, crates/napple-account/src/{profiles,photos,nicknames}.rs, tests/{profiles,photos}.rs,
change note. Edited (small): account-crate Cargo.toml (+axum multipart, rusqlite functions, image, napple-visibility), db.rs (1 call to
`profiles::migrate`), lib.rs (mod lines + `photos` in build_app), app.rs (`photos` field, profile routes re-pointed, photo route
with 5.25 MiB body limit, internal routes), account.rs (`delete_me` also deletes photos), tests/saves.rs ("Player {i}" -> "Player_{i}",
spaces are no longer legal), docs/guides/napple-account.md (Profiles section). Shared: root Cargo.toml (member), Cargo.lock.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Profiles section | tested | `cargo test -j2 -p napple-account -p napple-visibility`: all green (account: 25 unit + auth 9, keys 3, names 3, photos 15, profiles 7, saves 6, s3 1; visibility 7) |
| Photo pipeline + hostile inputs | tested | tests/photos.rs: bombs (forged IHDR 60000x60000), 8193 px, polyglots, truncated/noise, EXIF GPS/ICC/XMP/COM/PNG text bytes gone, orientation, wrong ext, SVG/GIF/HEIC/AVIF/BMP, zero size, >5 MiB |
| can_view shared fn | tested | crates/napple-visibility, full table + blocked + owner |
| S3 photo store | tested vs mock only | s3_store_signs_private_requests (SigV4 headers, no presign) |
| clippy | verified | clean on both crates, --all-targets |

## Unbuilt / untested list
- S3PhotoStore against real S3 (needs bucket; only MockOutbound).

## Findings / decisions
- Nickname uniqueness is enforced in `profiles::insert` and `patch_profile` (check + write under the one DB lock, no await), NOT by a
  UNIQUE index, because the legacy email sign-in in signin.rs inserts its own first profile (fallback "Player") and an index
  made it 500. **account-auth must create the first profile with `napple_account::profiles::insert(db, account_id, nickname, 0, now)`
  after `nicknames::validate`** (409 `nickname_taken` on collision; friend code is added by a SQLite trigger even for raw INSERTs).
  Once signin.rs no longer inserts profiles directly, a `CREATE UNIQUE INDEX ... nickname COLLATE NOCASE` can be added.
- Old handlers `account::{list,create,get,patch,delete}_profile` are dead code now (left to avoid conflicts with the auth session); delete when convenient.
- JPEG truncation: the decoder pads missing scan data with grey, so `photos::jpeg_complete` validates marker framing (SOI..EOI) first.
- Photo GET by other accounts has no friend data: tiers 1-2 only; napple-social uses `/internal/profiles/{id}/photo` + `can_view` for friends.
- Internal routes need env `NAPPLE_INTERNAL_TOKEN` (>=16 chars, header `x-napple-internal`); infra must keep `/internal/*` off the public Caddy.
- Confusable nicknames (skeleton collisions like "l" vs "I") are not rejected; only case-insensitive equality is (contract wording).
- Legacy DB with duplicate nicknames: no problem (no index). Existing profile rows are migrated (columns + friend codes).

## How to integrate
- Conflict hotspots: app.rs router (routes), lib.rs build_app, db.rs init, account.rs delete_me, tests/common (untouched). Resolve by keeping both sides.
- Build: `cargo build -j2 -p napple-account`; test: `cargo test -j2 -p napple-account -p napple-visibility`.
- napple-social should add `napple-visibility = { path = "../napple-visibility" }`.

## How to test
1. `cargo test -j2 -p napple-account -p napple-visibility` -> all ok.
2. Manual: run the server, POST a multipart JPEG with GPS EXIF to /account/v1/profiles/{id}/photo, GET it back, `exiftool` shows none, 256x256.

## Fix policy hints
Safe: import/signature drift after merging the auth session's changes. Needs a decision: adding the UNIQUE index (see Findings).

## Needs from other areas
- account-auth: use `profiles::insert` for the first profile (above). napple-infra: Caddy must not expose `/account/v1/internal/*`; set `NAPPLE_PHOTO_BUCKET` (private) and `NAPPLE_INTERNAL_TOKEN`. napple-social: depend on napple-visibility and the internal routes.
