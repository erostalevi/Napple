//! napple-cloud: the client side of the Napple Account (orchestrations/ACCOUNT-CONTRACT.md, docs/guides/cloud-saves.md).
//!
//! - `store`: secrets (tokens, the device X25519 key, the account save key) in the Keychain on macOS/iOS, files elsewhere.
//! - `keys`: HPKE wrap/unwrap of the account save key, recovery code (BIP-39 words + Argon2id).
//! - `tarball` + `blob`: deterministic tar of a title's save, zstd, AES-256-GCM under a per-version HKDF key.
//! - `api`: the account HTTP API; `session`: tokens, refresh, the account key.
//! - `sync`: pull before mount, push after commit, conflicts (keep both), plan limit (402), offline queue.
//! - `ffi`: the C API the native runtime calls (include/napple_cloud.h); `stub`: a local server for tests (feature `stub`).
pub mod api;
pub mod blob;
pub mod error;
pub mod ffi;
pub mod keys;
pub mod savedir;
pub mod session;
pub mod state;
pub mod store;
pub mod sync;
pub mod tarball;
pub mod util;

#[cfg(any(test, feature = "stub"))]
pub mod stub;

#[cfg(test)]
mod tests_e2e;

pub use error::{Error, Result};
