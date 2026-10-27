//! The `.napmod` community mod and fix format: portable core (no macOS dependencies).
//!
//! A `.napmod` is a zip with `mod.json`, `manifest.sha256` and any of `patches/`, `cheats/`, `files/` (romfs overlay),
//! `config.json` and `shaders/`. See docs/mods/format.md. Mods are unsigned by default, code patches need an explicit
//! enable, and nothing in this crate downloads anything.
pub mod apply;
pub mod buildglue;
pub mod cheats;
pub mod cli;
pub mod config;
pub mod ips;
pub mod manifest;
pub mod model;
pub mod pack;
pub mod pchtxt;
pub mod romfs;
pub mod shader;
pub mod source;
pub mod util;
pub mod validate;

pub use apply::{ApplyReport, ModuleBytes, apply_mods_to_module, dry_run_mods_for_module};
pub use cli::run;
pub use model::{LoadedMod, ResolvedMod, Selection};
pub use util::{ModError, Result};
