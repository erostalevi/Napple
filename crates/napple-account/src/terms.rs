//! Terms and age acceptance: signup requires `age_ok` (13+) and `terms_ok`; the acceptance is stored on the account
//! with the terms version and the time, so a later terms change can ask again.

use crate::error::{ApiError, ApiResult};
use rusqlite::{Connection, params};

/// Bump when the terms or privacy policy change materially.
pub const TERMS_VERSION: &str = "2026-11-03";

pub fn require(age_ok: bool, terms_ok: bool) -> ApiResult<()> {
    if !age_ok || !terms_ok {
        return Err(
            ApiError::bad_request("you must be 13 or older and accept the Terms and Privacy Policy")
                .with("error_code", "terms_required"),
        );
    }
    Ok(())
}

pub fn record(db: &Connection, account_id: &str, now: i64) -> rusqlite::Result<()> {
    db.execute(
        "UPDATE accounts SET age_ok_at = ?2, terms_ok_at = ?2, terms_version = ?3 WHERE id = ?1",
        params![account_id, now, TERMS_VERSION],
    )?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn both_boxes_required() {
        assert!(require(true, true).is_ok());
        assert!(require(false, true).is_err());
        assert!(require(true, false).is_err());
    }
}
