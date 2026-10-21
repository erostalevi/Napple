/// Error messages must never contain key material.
#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("key file: {0}")]
    KeyFile(String),
    #[error("required key not found: {0}")]
    MissingKey(String),
    #[error("container: {0}")]
    Container(String),
    #[error("unsupported: {0}")]
    Unsupported(String),
    #[error("integrity check failed: {0} (wrong keys or corrupt file)")]
    Integrity(String),
    #[error("io: {0}")]
    Io(#[from] std::io::Error),
}

pub type Result<T> = std::result::Result<T, Error>;

pub fn container<T>(msg: &str) -> Result<T> {
    Err(Error::Container(msg.to_string()))
}
