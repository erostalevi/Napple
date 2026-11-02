use std::fmt;

#[derive(Debug, Clone)]
pub enum Error {
    /// Not signed in (no account.json or no refresh token): every sync entry point is a no-op.
    NotSignedIn,
    /// Signed in, but no other device has granted this one the account save key yet (and no recovery code was entered).
    NoAccountKey,
    /// The refresh token was rejected: the user has to sign in again.
    Unauthorized,
    /// 409 on a new save version: `head` is the server's current version.
    Conflict { head: u64 },
    /// 402 {reason: "plan_limit", upgrade_url}: the plan does not cover cloud saves for this title.
    PlanLimit { upgrade_url: String },
    /// Any other HTTP error status.
    Http { status: u16, body: String },
    /// Could not reach the server (offline, DNS, TLS, timeout).
    Net(String),
    Crypto(String),
    Format(String),
    Io(String),
    Timeout,
}

pub type Result<T> = std::result::Result<T, Error>;

impl Error {
    /// Errors worth retrying later (the offline queue keeps the push).
    pub fn is_transient(&self) -> bool {
        matches!(self, Error::Net(_) | Error::Timeout) || matches!(self, Error::Http { status, .. } if *status >= 500 || *status == 429)
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::NotSignedIn => write!(f, "not signed in to a Napple Account (napple account login)"),
            Error::NoAccountKey => write!(
                f,
                "this device has no cloud save key yet: approve it from a signed-in device (napple account approve) or enter the recovery code (napple account recover)"
            ),
            Error::Unauthorized => write!(f, "the session expired or the device was revoked: sign in again (napple account login)"),
            Error::Conflict { head } => write!(f, "the cloud has a newer save (version {head})"),
            Error::PlanLimit { upgrade_url } => write!(f, "not synced (plan limit): your plan covers fewer titles; upgrade at {upgrade_url}"),
            Error::Http { status, body } => write!(f, "server error {status}: {body}"),
            Error::Net(e) => write!(f, "offline or server unreachable: {e}"),
            Error::Crypto(e) => write!(f, "crypto: {e}"),
            Error::Format(e) => write!(f, "format: {e}"),
            Error::Io(e) => write!(f, "io: {e}"),
            Error::Timeout => write!(f, "timed out"),
        }
    }
}

impl std::error::Error for Error {}

impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        Error::Io(e.to_string())
    }
}

impl From<serde_json::Error> for Error {
    fn from(e: serde_json::Error) -> Self {
        Error::Format(e.to_string())
    }
}
