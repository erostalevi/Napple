//! Errors of the rewriter. Per-site problems are not errors: they are collected as [`Unhandled`]
//! entries so a run reports every unsupported pattern at once.
use crate::a64::EncodeError;
use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RewriteError {
    /// The module image cannot be laid out (hostile or unsupported NSO).
    Layout(String),
    /// An encoding limit was hit (for instance a branch out of range of the patch area).
    Encode(String),
    /// Dynamic section / relocation tables are malformed.
    Relocation(String),
    /// Input/output problem in the CLI layer.
    Io(String),
    /// Bad command line.
    Usage(String),
}

impl fmt::Display for RewriteError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Layout(m) => write!(f, "layout: {m}"),
            Self::Encode(m) => write!(f, "encoding: {m}"),
            Self::Relocation(m) => write!(f, "relocation: {m}"),
            Self::Io(m) => write!(f, "io: {m}"),
            Self::Usage(m) => write!(f, "{m}"),
        }
    }
}

impl std::error::Error for RewriteError {}

impl From<EncodeError> for RewriteError {
    fn from(e: EncodeError) -> Self {
        Self::Encode(e.0)
    }
}

/// A site the rewriter could not handle; the original instruction is left in place.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Unhandled {
    pub addr: u32,
    pub word: u32,
    pub reason: String,
}

impl fmt::Display for Unhandled {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:#x} ({:08x}): {}", self.addr, self.word, self.reason)
    }
}
