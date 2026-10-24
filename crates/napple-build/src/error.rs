//! Build errors carry a stable machine-readable code (printed as `error: [code] message`).
use napple_core::domain::error::Error as CoreError;
use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BuildError {
    pub code: &'static str,
    pub msg: String,
}

pub type Result<T> = std::result::Result<T, BuildError>;

impl BuildError {
    pub fn new(code: &'static str, msg: impl Into<String>) -> Self {
        Self { code, msg: msg.into() }
    }
    pub fn usage(msg: impl Into<String>) -> Self {
        Self::new("usage", msg)
    }
    pub fn io(ctx: &str, e: std::io::Error) -> Self {
        let code = if e.raw_os_error() == Some(libc::ENOSPC) { "disk-space" } else { "io" };
        Self::new(code, format!("{ctx}: {e}"))
    }
    pub fn cancelled() -> Self {
        Self::new("cancelled", "build cancelled")
    }
    pub fn is_usage(&self) -> bool {
        self.code == "usage"
    }
}

impl fmt::Display for BuildError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "[{}] {}", self.code, self.msg)
    }
}

impl std::error::Error for BuildError {}

impl From<CoreError> for BuildError {
    fn from(e: CoreError) -> Self {
        // Core errors never contain key material (see napple-core::domain::error).
        let code = match &e {
            CoreError::KeyFile(_) | CoreError::MissingKey(_) => "keys",
            CoreError::Integrity(_) => "integrity",
            CoreError::Unsupported(_) => "unsupported",
            CoreError::Io(io) if io.raw_os_error() == Some(libc::ENOSPC) => "disk-space",
            CoreError::Io(_) => "io",
            CoreError::Container(_) => "container",
        };
        Self::new(code, e.to_string())
    }
}

impl From<std::io::Error> for BuildError {
    fn from(e: std::io::Error) -> Self {
        Self::io("io", e)
    }
}
