# 2026-11-02 cloud-saves-client (r8): Napple Account client and end-to-end encrypted cloud saves

Branch `claude/cloud-saves-client-2026-11-02-r8` (orchestration `cloud-saves-client`, batch 2026-11-02-r8). User guide and wire details:
`docs/guides/cloud-saves.md`. The server is the parallel `napple-account` item; this branch tests against a local stub of the contract.

## What changed
- **`crates/napple-cloud`** (new, Rust; rlib for the CLI + staticlib for the runtime):
  - `store`: secrets in the Keychain (macOS login keychain with an any-application decrypt ACL and no UI, or a keychain file via
    `NAPPLE_ACCOUNT_KEYCHAIN`; iOS after-first-unlock this-device-only), 0600 files with `NAPPLE_ACCOUNT_STORE=file`.
  - `keys`: X25519 device key, HPKE base-mode wrap/unwrap of the account save key (RFC 9180 X25519/HKDF-SHA256/AES-256-GCM), 24-word
    recovery code with Argon2id.
  - `tarball` + `blob`: deterministic ustar of the save, zstd, AES-256-GCM under HKDF(account key, `napple-save|title|profile|version`)
    with a synthetic (HMAC) nonce: blob format 1 (`NSAV`).
  - `session`: email sign-in, token refresh with rotation, first-device key creation, device approval, recovery, profiles and links, logout.
  - `sync`: pull before mount, push after commit, conflict = keep both (remote current, local restore point, event), 402 plan limit
    (status `plan_limit`, upgrade URL, save stays local), offline queue with retry, history, restore of a version or a restore point.
  - `ffi` + `include/napple_cloud.h`: the runtime API (start = pull before mount + background thread; commit = debounced push; flush on exit;
    status JSON; event callback).
  - `stub` (feature `stub`, binary `napple-cloud-stub`): an in-memory implementation of the contract for tests and local development.
- **Runtime** (`runtime/src/cloud/cloud.c`, hooks): `main.c` calls `cloud_runtime_start()` after the title id is known and before
  `services_init`; `services/fs.c` calls `cloud_on_commit(root)` after a commit that took a new snapshot and `cloud_on_exit()` from
  `fs_report_uncommitted` (both clean exits). Weak no-op defaults in `main.c`/`fs.c` keep builds without `runtime/src/cloud` linking
  (`make CLOUD=0`, `tools/build_ios.sh` as it is). `runtime/Makefile` (appended) builds `target/release/libnapple_cloud.a` with cargo and links
  it (+ Security) into the runtime and the applet dylib only (not the unit tests).
- **CLI**: `napple account login|logout|status|devices|approve|recover|profiles|link|plan|upgrade`, `napple saves list|pull|push|history|restore|enable|disable`.
- Docs: `docs/guides/cloud-saves.md` (usage, plans, security model, wire details, blob format, environment).

## Status
See the integrator file `integrators/claude-cloud-saves-client-2026-11-02-r8.md` for the verified/tested list and evidence.
