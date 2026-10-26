//! napple-rewrite: ahead-of-time rewriter for Switch NSO modules.
pub mod a32;
pub mod a64;
pub mod check;
pub mod cli;
pub mod error;
pub mod image;
pub mod jumptab;
pub mod output;
pub mod patch;
pub mod relocs;
pub mod rewrite;
pub mod scan;
pub mod survey;
pub mod templates;
pub mod types;
pub mod windows;

pub use rewrite::rewrite_module;
pub use types::{Gate, GateKind, Key, RewriteOptions, RewriteOutput, SiteRow};

pub use cli::run;
