//! Auth schema additions and the emailed 6-digit codes: single use, 10 minutes, at most 5 attempts per code,
//! HMAC-stored (the database never holds a usable code). One live code per (email, purpose); a new one replaces it.

use crate::app::{AppState, limit};
use crate::error::{ApiError, ApiResult};
use crate::mailer::{CodeKind, Mail, MailError, render_code_mail};
use crate::util::{ct_eq, hmac_sha256, random_code};
use rusqlite::{Connection, OptionalExtension, params};

pub const VERIFY: &str = "verify";
pub const RESET: &str = "reset";

const SCHEMA: &str = r#"
CREATE TABLE IF NOT EXISTS auth_codes (
    email TEXT NOT NULL,
    purpose TEXT NOT NULL,
    code_mac BLOB NOT NULL,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    attempts INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (email, purpose)
);
CREATE TABLE IF NOT EXISTS login_failures (
    email TEXT PRIMARY KEY,
    window_start INTEGER NOT NULL,
    count INTEGER NOT NULL,
    locked_until INTEGER NOT NULL
);
-- Addresses the mail provider reported as permanently undeliverable (bounce, complaint, suppression list).
CREATE TABLE IF NOT EXISTS mail_suppressions (
    email TEXT PRIMARY KEY,
    reason TEXT NOT NULL,
    created_at INTEGER NOT NULL
);
DROP TABLE IF EXISTS passkeys;
DROP TABLE IF EXISTS webauthn_challenges;
DROP TABLE IF EXISTS email_challenges;
"#;

/// Columns added to `accounts` for email + password sign-in. Idempotent; runs on every open.
const ACCOUNT_COLUMNS: &[(&str, &str)] = &[
    ("password_hash", "TEXT"),
    ("verified_at", "INTEGER"),
    ("pending_nickname", "TEXT"),
    ("pending_device", "TEXT"),
    ("age_ok_at", "INTEGER"),
    ("terms_ok_at", "INTEGER"),
    ("terms_version", "TEXT"),
];

pub fn migrate(c: &Connection) -> rusqlite::Result<()> {
    c.execute_batch(SCHEMA)?;
    let have: Vec<String> = c
        .prepare("SELECT name FROM pragma_table_info('accounts')")?
        .query_map([], |r| r.get(0))?
        .collect::<Result<_, _>>()?;
    let mut added = false;
    for (name, ty) in ACCOUNT_COLUMNS {
        if !have.iter().any(|h| h == name) {
            c.execute(&format!("ALTER TABLE accounts ADD COLUMN {name} {ty}"), [])?;
            added = true;
        }
    }
    if added {
        // Accounts created by the earlier magic-link sign-in already proved their address.
        c.execute(
            "UPDATE accounts SET verified_at = created_at WHERE verified_at IS NULL AND id IN (SELECT account_id FROM devices)",
            [],
        )?;
    }
    Ok(())
}

fn mac(app: &AppState, email: &str, purpose: &str, code: &str) -> Vec<u8> {
    hmac_sha256(&app.code_key, format!("{purpose}|{email}|{code}").as_bytes()).to_vec()
}

pub fn is_suppressed(db: &Connection, email: &str) -> rusqlite::Result<bool> {
    Ok(db
        .query_row("SELECT 1 FROM mail_suppressions WHERE email = ?1", [email], |_| Ok(()))
        .optional()?
        .is_some())
}

/// Per-address and per-IP limits for mails that carry a code: 5 per hour per address, 20 per hour per IP.
/// Applied before anything else so the answer never depends on whether the account exists.
pub fn check_mail_limits(app: &AppState, email: &str, ip: &str) -> ApiResult<()> {
    limit(app, "codemail-ip", ip, app.cfg.code_mails_per_ip_hour, 3600)?;
    limit(app, "codemail-email", email, app.cfg.code_mails_per_email_hour, 3600)
}

/// Creates a fresh code for (email, purpose) and mails it. A suppressed or newly-undeliverable address is recorded
/// and silently skipped: the caller answers the same way as for a delivered mail.
pub async fn send_code(app: &AppState, email: &str, purpose: &str) -> ApiResult<()> {
    let now = app.now();
    if is_suppressed(&app.db(), email)? {
        return Ok(());
    }
    let code = random_code();
    app.db().execute(
        "INSERT OR REPLACE INTO auth_codes (email, purpose, code_mac, created_at, expires_at, attempts) VALUES (?1, ?2, ?3, ?4, ?5, 0)",
        params![email, purpose, mac(app, email, purpose, &code), now, now + app.cfg.email_code_ttl],
    )?;
    let kind = if purpose == RESET { CodeKind::Reset } else { CodeKind::Confirm };
    let (subject, text, html) = render_code_mail(kind, &code, app.cfg.email_code_ttl / 60);
    let sent = app
        .mailer
        .send(Mail { to: email.to_string(), subject, text, html: Some(html) }, now)
        .await;
    match sent {
        Ok(()) => Ok(()),
        Err(MailError::Undeliverable(why)) => {
            crate::log::warn(&format!("mail undeliverable, address suppressed: {why}"));
            let db = app.db();
            db.execute(
                "INSERT OR IGNORE INTO mail_suppressions (email, reason, created_at) VALUES (?1, ?2, ?3)",
                params![email, why, now],
            )?;
            db.execute("DELETE FROM auth_codes WHERE email = ?1", [email])?;
            Ok(())
        }
        Err(MailError::Failed(e)) => {
            crate::log::error(&format!("mailer: {e}"));
            Err(ApiError::unavailable("mail_failed", "could not send the email, try again"))
        }
    }
}

pub fn invalid() -> ApiError {
    ApiError::bad_request("invalid or expired code").with("error_code", "invalid_code")
}

/// Checks and consumes a code. Wrong code: attempt counted; after the limit the code is gone. Unknown, expired and
/// wrong all answer the same `invalid_code`.
pub fn consume(app: &AppState, email: &str, purpose: &str, code: &str) -> ApiResult<()> {
    let now = app.now();
    let db = app.db();
    let row: Option<(Vec<u8>, i64, u32)> = db
        .query_row(
            "SELECT code_mac, expires_at, attempts FROM auth_codes WHERE email = ?1 AND purpose = ?2",
            params![email, purpose],
            |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
        )
        .optional()?;
    let Some((stored, expires_at, attempts)) = row else {
        return Err(invalid());
    };
    if now > expires_at || attempts >= app.cfg.email_code_attempts {
        db.execute("DELETE FROM auth_codes WHERE email = ?1 AND purpose = ?2", params![email, purpose])?;
        return Err(invalid());
    }
    if !ct_eq(&mac(app, email, purpose, code.trim()), &stored) {
        db.execute(
            "UPDATE auth_codes SET attempts = attempts + 1 WHERE email = ?1 AND purpose = ?2",
            params![email, purpose],
        )?;
        return Err(invalid());
    }
    db.execute("DELETE FROM auth_codes WHERE email = ?1 AND purpose = ?2", params![email, purpose])?;
    Ok(())
}
