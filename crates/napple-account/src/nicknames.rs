//! Profile nicknames: 3-20 chars of `[A-Za-z0-9_.-]`, globally unique ignoring case (checked case-insensitively under the
//! connection lock in `profiles::insert` / `patch_profile`; the charset is ASCII so NOCASE is exact), plus the shared reserved-name rules of `names.rs`.

use crate::error::{ApiError, ApiResult};
use crate::names::{NameError, check_name};
use axum::http::StatusCode;
use rusqlite::Connection;

pub const MIN_LEN: usize = 3;
pub const MAX_LEN: usize = 20;

pub fn valid_charset(n: &str) -> bool {
    (MIN_LEN..=MAX_LEN).contains(&n.len())
        && n.bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'_' | b'.' | b'-'))
}

/// Shape and reserved-name checks (not uniqueness): returns the nickname exactly as typed.
pub fn validate(db: &Connection, account_id: &str, raw: &str) -> ApiResult<String> {
    let grants = crate::admin::grants_of(db, account_id)?;
    check_name(raw, 32, &grants).map_err(|e| match e {
        NameError::ReservedWord | NameError::ReservedPrefix => ApiError::new(
            StatusCode::UNPROCESSABLE_ENTITY,
            "reserved_name",
            e.message(),
        )
        .with("reason", "reserved_name")
        .with("field", "nickname"),
        NameError::Invalid | NameError::Invisible => {
            ApiError::bad_request(e.message()).with("field", "nickname")
        }
    })?;
    if !valid_charset(raw) {
        return Err(ApiError::bad_request(
            "nickname: 3-20 characters, letters, digits, '_', '.' and '-' only",
        )
        .with("field", "nickname"));
    }
    Ok(raw.to_string())
}

/// True when another profile (not `except`) already uses the nickname, ignoring case.
pub fn taken(db: &Connection, nick: &str, except: Option<&str>) -> rusqlite::Result<bool> {
    db.query_row(
        "SELECT EXISTS(SELECT 1 FROM profiles WHERE nickname = ?1 COLLATE NOCASE AND id != ?2)",
        rusqlite::params![nick, except.unwrap_or("")],
        |r| r.get(0),
    )
}

pub fn taken_error() -> ApiError {
    ApiError::conflict("nickname_taken", "that nickname is taken").with("field", "nickname")
}

/// Maps a unique-index violation on `profiles.nickname` to the 409 (races between the check and the write).
pub fn map_insert_error(e: rusqlite::Error) -> ApiError {
    if let rusqlite::Error::SqliteFailure(f, Some(m)) = &e
        && f.code == rusqlite::ErrorCode::ConstraintViolation
        && m.contains("profiles.nickname")
    {
        return taken_error();
    }
    e.into()
}

/// Search prefixes use the nickname charset (1-20 chars).
pub fn valid_prefix(p: &str) -> bool {
    !p.is_empty()
        && p.len() <= MAX_LEN
        && p.bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'_' | b'.' | b'-'))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn charset_and_length() {
        for ok in ["abc", "A_b.c-d", "x".repeat(20).as_str(), "Neto2026"] {
            assert!(valid_charset(ok), "{ok}");
        }
        for bad in [
            "ab",
            "x".repeat(21).as_str(),
            "has space",
            "caf\u{e9}",
            "a\u{0}bc",
            "emoji\u{1F600}",
            "a/b",
            "",
            "tab\tbed",
        ] {
            assert!(!valid_charset(bad), "{bad:?}");
        }
    }
}
