---
name: account-profiles
batch: 2026-11-03-r12
branch: claude/account-profiles-2026-11-03-r12
base: orchestrate/2026-11-03-r12
env: mac
model: claude-sonnet-5-5
depends_on: []
status: launched
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_307ebf20-1598-430d-b883-d38a21efa937
launched_at: 2026-11-03
---

# account-profiles: profiles: unique nicknames, bio, links, icons, sanitized photo upload, visibility tier (napple-account profile side)

Read `orchestrations/ONLINE-CONTRACT.md` and `orchestrations/ENV-MAC.md` first; the contract is the source of truth for APIs, env names, tiers and the C API.

## Owned paths
new `crates/napple-account/src/{profiles,photos,nicknames}.rs` + `names.rs` (existing reserved-name code), `crates/napple-account/tests/{profiles,photos}*.rs`, `docs/guides/napple-account.md` (profile sections), change note. Router/db edits in `app.rs`/`db.rs`: small, append-only (another session edits auth files in the same crate).

## Do not touch
Everything else: other r12 items edit the other paths in parallel (account auth/profiles, social, online server, client core, overlay, MK8D protocol, infra, legal), `orchestrations/`. Shared files (`Cargo.toml` members, `Cargo.lock`, `runtime/Makefile`, `docs/index.md`, `docs/archive/changes/README.md`): append-only and minimal. Use `cargo build -j2` / `cargo test -j2` (several sessions build at once on a 16 GB Mac).

## Work and acceptance
1. Implement the Profiles section of the contract: global unique nicknames (case-insensitive) + reserved names, bio, links (https only for web), icon_id, `tier` 1..5, friend code, up to 8 profiles/account, profile CRUD.
2. Photo pipeline exactly as specified (size/pixel caps, magic-byte type check, full decode, strip metadata, center-crop, 256x256, re-encode JPEG q85, private storage in `NAPPLE_PHOTO_DIR` with an S3 backend behind a trait for `NAPPLE_PHOTO_BUCKET`), `GET` honouring the tier table via a pure function `can_view(viewer, target_profile, kind)` shared with napple-social (put it in a small crate or module social can depend on: coordinate by writing it as `crates/napple-visibility` with just the tier rules + tests).
3. Tests with hostile inputs: decompression bomb, polyglot files, truncated/corrupt images, EXIF GPS stripped (verify bytes), wrong extension, huge dimensions, SVG/GIF/HEIC rejected, zero-size.
4. `cargo test -p napple-account -p napple-visibility` green.

## Env notes
Shared title data read-only; game runs under `lockf -k /Users/neto/NappleWork/.run.lock` (<=120 s, private --save-root); local only, never push; never ask the user; never commit credentials or Nintendo material. Test-first, small commits.

## Result
