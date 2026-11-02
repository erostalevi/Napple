//! Password policy and Argon2id hashing. Hashes are PHC strings (parameters travel with the hash, so the work
//! factor can be raised later and old hashes still verify).

use argon2::password_hash::{PasswordHash, PasswordHasher, PasswordVerifier, SaltString};
use argon2::{Algorithm, Argon2, Params, Version};

pub const MIN_LEN: usize = 10;
pub const MAX_LEN: usize = 128;

/// Production work factor: m = 64 MiB, t = 3, p = 1. `m_kib` is configurable for fast tests only.
pub const DEFAULT_M_KIB: u32 = 64 * 1024;

const COMMON: &str = include_str!("common_passwords.txt");

#[derive(Debug, PartialEq, Eq)]
pub enum PolicyError {
    TooShort,
    TooLong,
    Common,
    Email,
}

impl PolicyError {
    pub fn message(&self) -> &'static str {
        match self {
            PolicyError::TooShort => "password: at least 10 characters",
            PolicyError::TooLong => "password: at most 128 characters",
            PolicyError::Common => "password: too common, choose another",
            PolicyError::Email => "password: must not be your email address",
        }
    }
}

/// Length counts characters, not bytes. Case-insensitive match against the bundled common-password list, and the
/// address (or its local part) itself is refused.
pub fn check(password: &str, email: &str) -> Result<(), PolicyError> {
    let n = password.chars().count();
    if n < MIN_LEN {
        return Err(PolicyError::TooShort);
    }
    if n > MAX_LEN {
        return Err(PolicyError::TooLong);
    }
    let lower = password.to_lowercase();
    if COMMON.lines().any(|l| l.trim() == lower) {
        return Err(PolicyError::Common);
    }
    let local = email.split('@').next().unwrap_or("");
    if lower == email.to_lowercase() || (local.chars().count() >= MIN_LEN && lower == local.to_lowercase()) {
        return Err(PolicyError::Email);
    }
    // A single repeated character or a plain run is as bad as a listed password.
    let first = lower.chars().next();
    if lower.chars().all(|c| Some(c) == first) {
        return Err(PolicyError::Common);
    }
    Ok(())
}

fn argon(m_kib: u32) -> Argon2<'static> {
    let params = Params::new(m_kib, 3, 1, None).expect("argon2 params");
    Argon2::new(Algorithm::Argon2id, Version::V0x13, params)
}

pub fn hash(password: &str, m_kib: u32) -> Result<String, String> {
    let salt = SaltString::generate(&mut rand::rngs::OsRng);
    argon(m_kib)
        .hash_password(password.as_bytes(), &salt)
        .map(|h| h.to_string())
        .map_err(|e| e.to_string())
}

/// Verifies against a stored PHC string with the parameters it carries.
pub fn verify(password: &str, phc: &str) -> bool {
    match PasswordHash::new(phc) {
        Ok(h) => Argon2::default()
            .verify_password(password.as_bytes(), &h)
            .is_ok(),
        Err(_) => false,
    }
}

/// A valid hash of a random password, used to spend the same time when the account does not exist (no timing oracle).
pub fn dummy_hash(m_kib: u32) -> String {
    hash(&crate::util::random_token(), m_kib).unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn policy() {
        let e = "player@example.com";
        assert_eq!(check("short", e), Err(PolicyError::TooShort));
        assert_eq!(check("Password123", e), Err(PolicyError::Common));
        assert_eq!(check("QWERTYUIOP", e), Err(PolicyError::Common));
        assert_eq!(check("aaaaaaaaaaaa", e), Err(PolicyError::Common));
        assert_eq!(check(&"x".repeat(129), e), Err(PolicyError::TooLong));
        assert_eq!(check("player@example.com", e), Err(PolicyError::Email));
        assert_eq!(check("correct horse battery", e), Ok(()));
        assert_eq!(check("ünïcödé-pässwört", e), Ok(()));
    }

    #[test]
    fn hashes_are_argon2id_with_the_production_parameters() {
        let h = hash("correct horse battery", DEFAULT_M_KIB).unwrap();
        assert!(h.starts_with("$argon2id$v=19$m=65536,t=3,p=1$"), "{h}");
        assert!(verify("correct horse battery", &h));
        assert!(!verify("correct horse batterz", &h));
        assert!(!verify("x", "not a hash"));
        assert_ne!(h, hash("correct horse battery", DEFAULT_M_KIB).unwrap(), "salted");
    }
}
