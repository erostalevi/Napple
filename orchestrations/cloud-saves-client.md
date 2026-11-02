---
name: cloud-saves-client
batch: 2026-11-02-r8
branch: claude/cloud-saves-client-2026-11-02-r8
base: orchestrate/2026-11-02-r8
env: mac
model: claude-opus-5-5
depends_on: []
status: planned
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: none
launched_at: none
---

# cloud-saves-client: account sign-in and end-to-end encrypted save sync in the runtime and the napple CLI (Mac + iOS)

## Goal
Client side of `orchestrations/ACCOUNT-CONTRACT.md`, shared by the macOS runtime, the iOS runtime and the `napple` CLI:
- A portable sync core (C or Rust FFI, your call; macOS/iOS crypto via CryptoKit/CommonCrypto or a vetted Rust crate) : token store in the Keychain
  (macOS + iOS), device X25519 key in the Keychain (Secure Enclave-backed where available), HPKE unwrap of the account save key, deterministic tar +
  zstd + AES-256-GCM save blobs, version bookkeeping (`base_version`), conflict handling (keep both: the remote becomes current, the local copy is kept
  as a restore point, and the frontend is told).
- Runtime hooks: on title launch pull the newest version for (title, linked profile) BEFORE the game mounts its save; push after the game commits
  its save filesystem (debounced, background, never blocking the game thread) and on clean exit; offline-first: queue and retry, never lose a local save.
- CLI: `napple account login|logout|status|devices|profiles|link`, `napple saves list|pull|push|history|restore`.
- Opt-in: nothing syncs unless signed in and the title is enabled (default on after sign-in, per-title toggle).

## Owned paths
New `runtime/src/cloud/` (+ its tests), minimal hooks in `runtime/src/services/fs.c` / `runtime/src/main.c` save-root code (document each hook),
`crates/napple-cli/` (account + saves subcommands) and a new `crates/napple-cloud/` if you choose Rust for the core, `frontend/src/ios/` only for the
iOS hook/Keychain glue, `docs/guides/cloud-saves.md` (new), own change note. `runtime/Makefile`, workspace `Cargo.toml`: append-only.

## Do not touch
`crates/napple-account/` (server, parallel session — test against its contract; use a local stub server in your tests), `crates/napple-online/`,
`crates/napple-room/`, `runtime/src/net/`, `studio/` (UI comes next round), `infra/`, `orchestrations/`.

## Acceptance
1. Unit tests: blob format round trip, HKDF/AES-GCM vectors, HPKE unwrap vectors, tar determinism, conflict path, offline queue, Keychain wrapper (macOS).
2. End-to-end against a local stub of the contract (or the napple-account branch if it is ready — merge it into a scratch worktree for the check only):
   two "devices" (two Keychain namespaces) on this Mac: sign in, approve device B from A, play LAS on A (headless, scripted save), launch on B and the
   save is pulled before mount; edit on both -> conflict kept.
3. **Verified on a title**: SMO or LAS headless run on "Mac device A" saves, a second run as "device B" (separate save root) starts from that save.
4. iOS: builds (`tools/build_ios.sh` or the simulator target) with the hook; simulator run optional.
5. Signed-out behaviour identical to today; `make -C runtime test test-services` and cargo tests green; four-title boots no regression.

## Env notes
Read `orchestrations/ENV-MAC.md` (shared title data read-only, locks, local-only, never ask, never push).

## Result
