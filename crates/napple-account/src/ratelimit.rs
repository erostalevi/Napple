//! Failed-login lockout: 10 failures within 15 minutes lock the address for 15 minutes. Keyed by the normalised
//! email whether or not an account exists, so the lock (and its message) says nothing about who is registered.
//! Stored in the database so it survives a restart.

use rusqlite::{Connection, OptionalExtension, params};

pub const WINDOW: i64 = 15 * 60;
pub const LOCK: i64 = 15 * 60;

/// Seconds left on a lock, if the address is locked now.
pub fn locked_for(db: &Connection, email: &str, now: i64) -> rusqlite::Result<Option<u64>> {
    let until: Option<i64> = db
        .query_row(
            "SELECT locked_until FROM login_failures WHERE email = ?1",
            [email],
            |r| r.get(0),
        )
        .optional()?;
    Ok(until.filter(|u| *u > now).map(|u| (u - now) as u64))
}

/// Records one failed attempt; locks at `limit`.
pub fn fail(db: &Connection, email: &str, limit: u32, now: i64) -> rusqlite::Result<()> {
    let cur: Option<(i64, i64)> = db
        .query_row(
            "SELECT window_start, count FROM login_failures WHERE email = ?1",
            [email],
            |r| Ok((r.get(0)?, r.get(1)?)),
        )
        .optional()?;
    let (start, count) = match cur {
        Some((s, c)) if now - s < WINDOW => (s, c + 1),
        _ => (now, 1),
    };
    let locked_until = if count >= i64::from(limit) { now + LOCK } else { 0 };
    db.execute(
        "INSERT INTO login_failures (email, window_start, count, locked_until) VALUES (?1, ?2, ?3, ?4)
         ON CONFLICT(email) DO UPDATE SET window_start = ?2, count = ?3, locked_until = ?4",
        params![email, start, count, locked_until],
    )?;
    Ok(())
}

pub fn clear(db: &Connection, email: &str) -> rusqlite::Result<()> {
    db.execute("DELETE FROM login_failures WHERE email = ?1", [email])?;
    Ok(())
}
