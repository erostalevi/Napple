---
branch: claude/cloud-saves-client-2026-11-02-r8
orchestration: cloud-saves-client
base: 079dd49
head: (see git log; the commit that adds this file)
env: mac
date: 2026-11-02
areas: [crates/napple-cloud, crates/napple-cli, runtime/src/cloud, runtime hooks (main.c, services/fs.c), runtime/Makefile, docs]
status: ready-with-caveats
---

# claude/cloud-saves-client-2026-11-02-r8: Napple Account client + end-to-end encrypted cloud saves (runtime + CLI)

## Summary
New Rust crate `crates/napple-cloud` (rlib + staticlib): Keychain secret store, X25519 device key, HPKE wrap/unwrap of the account save key,
24-word recovery (Argon2id), deterministic tar + zstd + AES-256-GCM blobs (per-version HKDF key, synthetic nonce), sync engine (pull before
mount, push after commit, conflict = keep both, 413 quota, offline queue, history/restore), C FFI for the runtime, and an in-memory stub of the
contract (`napple-cloud-stub`). Runtime glue `runtime/src/cloud/` + minimal hooks in `main.c` / `services/fs.c` (weak no-op defaults). CLI
`napple account login|logout|status|devices|approve|recover|profiles|link`, `napple saves list|pull|push|history|restore|enable|disable`.
Guide `docs/guides/cloud-saves.md` (incl. wire details the contract leaves open).
Scope changes from the orchestrator during the session: billing/plans removed (402/upgrade dropped, 413 quota instead); overlay C API withdrawn.
NOT done (usage limit reached): iOS build check, `make -C runtime test test-services`, signed-out four-title boots, CLOUD=0 build check,
conflict path on the real title (conflict is tested only in the stub e2e test).

## Files
`git diff --stat 079dd49..HEAD`: new `crates/napple-cloud/**`, `crates/napple-cli/src/cloud.rs`, `runtime/src/cloud/{cloud.c,cloud.h}`,
`docs/guides/cloud-saves.md`, change note. Shared, append-only: workspace `Cargo.toml` (members + napple-cloud), `Cargo.lock` (additions; cargo
re-disambiguated a few dependency names), `runtime/Makefile` (appended block), `crates/napple-cli/{Cargo.toml,src/main.rs}` (2 dispatch lines + usage).
Hooks: `runtime/src/main.c` (+6), `runtime/src/services/fs.c` (+10).

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| 1. Unit tests: blob round trip, HKDF (RFC 5869) / AES-256-GCM / HPKE (RFC 9180 mode 0 X25519/SHA256/AES-256-GCM) vectors, tar determinism (+ system tar reads it, hostile paths rejected), conflict, offline queue, quota, Keychain wrapper (macOS keychain file) | tested | `cargo test -p napple-cloud --lib` -> 17 passed |
| 2. E2E vs local stub: sign in, approve B from A, pull before mount, edits on both -> conflict kept, refresh, recovery, revoke, restore, runtime FFI | tested (stub, in-process) + verified (CLI + runtime processes, real Keychain files) | `tests_e2e::two_devices_end_to_end`; CLI: `napple account login` A (first device, recovery code) / B (waiting) / `approve` from A / B `status` -> save key ready |
| 3. Real title: LAS on device A saves, device B starts from it | **verified** | LAS 140 s route on A (fresh save; `swkbd: entered "Link"`, 8 commits: savedata0/3.bin, screenshot3.bin); A run 30 s: `cloud: pushed ... version 1`; B run 108 s (empty save root): `cloud: pulled ... version 1 before mount (4 ms)` before the first `fs: mounted`; files byte-identical (cmp); B's file select shows A's File 1 "Link" + Autosave "Marin and Tarin's House" (shared notes `cloud-saves-client-evidence/las-deviceB-file-select.png`) |
| 4. iOS builds with the hook | untested | weak no-op hooks keep `tools/build_ios.sh` unchanged-linkable (not run); scratch variant with cloud + `aarch64-apple-ios` lib prepared (rust target installed) but not run |
| 5. Signed-out identical; runtime tests; four-title boots | partially: signed-out no-op tested (`signed_out_is_a_no_op`: returns 0, writes nothing); runtime test suites and four-title boots NOT run |
| macOS runtime link with libnapple_cloud.a | verified | `make -C runtime build/napple-runtime` rc 0, no warnings (MACOSX_DEPLOYMENT_TARGET=14.0 for the cargo step) |

## Unbuilt / untested list
- iOS: `runtime/src/cloud/cloud.c` + `libnapple_cloud.a` for aarch64-apple-ios never built/linked; `store.rs` iOS keychain branch unbuilt.
- `make -C runtime test test-services` (fs.c hook change) not run; `make CLOUD=0` not run.
- `napple saves restore/history` and `account recover/devices revoke` exercised in the stub test through the library, not through the CLI binary.

## Findings
- Bug found by the real run and fixed: the default local uid constant was wrong (`4e6170706c6555736572000000000001` is g_user_id); a test now parses account.c.
- Existing runtime quirk (not mine): NAPPLE_PROFILES users differ only in the last uid byte but fs.c names account saves by the first 4 bytes, so all local users share one save dir.
- macOS Keychain: dev binaries are unsigned, so items get an any-application decrypt ACL (no prompts across CLI/Studio/games); move to the data-protection keychain + access group once signed.
- Contract gaps decided client-side (guide "Wire details", shared note `cloud-saves-client-wire.md`, sent to napple-account): version = base_version + 1; `GET /keys/recovery`; base64url; 409 `{head}`; 413 `{reason:"quota"}`.

## How to integrate
- Independent of napple-account; align wire details with it (see above). Conflict hotspot: workspace `Cargo.toml` members line (both append) and `Cargo.lock`.
- Build: `make -C runtime` (needs cargo on PATH; builds `target/release/libnapple_cloud.a`); `cargo build --release -p napple-cli`.
- iOS: add `runtime/src/cloud/*.c`, `-Icrates/napple-cloud/include`, `target/aarch64-apple-ios/release/libnapple_cloud.a -framework Security` to `tools/build_ios.sh` (needs `rustup target add aarch64-apple-ios`).

## How to test
1. `cargo test -p napple-cloud` (17 pass). 2. `make -C runtime test test-services` (expect unchanged). 3. Stub: `cargo run --release -p napple-cloud --features stub --bin napple-cloud-stub -- 127.0.0.1:8787 --code 246810`; two devices = env `NAPPLE_ACCOUNT_URL`, `NAPPLE_SUPPORT_DIR`, `NAPPLE_ACCOUNT_NAMESPACE=devA|devB`, `NAPPLE_ACCOUNT_KEYCHAIN=<file>`, separate `--save-root`; login A, login B, `napple account approve` on A; LAS route (tools/golden/routes.json boot-to-village up to 128 s) on A, then 30 s A run (`cloud: pushed`), then B run (`cloud: pulled ... before mount`). 4. Signed-out four-title boots (no `cloud:` lines except none; rc 0).

## Fix policy hints
Link/include fixes in Makefile/iOS script are safe. Wire-format mismatches with napple-account need a decision (prefer changing the client's tolerant parsing).

## Needs from other areas
- `tools/build_ios.sh` (not owned): cloud sources + lib, as above.
- Overlay Account tab (later round) and iOS sign-in UI: the iOS runtime stays signed out (no-op) until then.
