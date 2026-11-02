//! Email magic link / 6-digit code sign-in, device registration at sign-in, token issue, refresh rotation, logout.

use crate::app::{AppState, Authed, ClientIp, limit};
use crate::error::{ApiError, ApiResult};
use crate::jwt::{AUDIENCE, Claims};
use crate::mailer::Mail;
use crate::util::{
    b64_decode, b64url, clean_name, ct_eq, hmac_sha256, new_id, normalize_email, random_code,
    random_token, token_hash,
};
use axum::Json;
use axum::extract::State;
use axum::http::StatusCode;
use percent_encoding::{NON_ALPHANUMERIC, utf8_percent_encode};
use rusqlite::{Connection, OptionalExtension, params};
use serde::Deserialize;
use serde_json::{Value, json};

pub const PLATFORMS: &[&str] = &["macos", "ios", "cli"];

#[derive(Deserialize)]
pub struct DeviceIn {
    pub name: String,
    pub platform: String,
    /// X25519 public key, 32 bytes, base64url.
    pub pubkey: String,
}

/// Validates a device and returns (name, platform, canonical pubkey).
pub fn check_device(d: &DeviceIn) -> ApiResult<(String, String, String)> {
    let name = clean_name(&d.name, 64)
        .ok_or_else(|| ApiError::bad_request("device.name: 1-64 characters"))?;
    let platform = d.platform.trim().to_ascii_lowercase();
    if !PLATFORMS.contains(&platform.as_str()) {
        return Err(ApiError::bad_request("device.platform: macos, ios or cli"));
    }
    let pk = b64_decode(&d.pubkey)
        .filter(|k| k.len() == 32 && k.iter().any(|&b| b != 0))
        .ok_or_else(|| ApiError::bad_request("device.pubkey: 32-byte X25519 key, base64url"))?;
    Ok((name, platform, b64url(&pk)))
}

/// Same account + same device public key = the same device signing in again (keeps its wrapped save key).
pub fn upsert_device(
    db: &Connection,
    account_id: &str,
    d: &DeviceIn,
    now: i64,
) -> ApiResult<String> {
    let (name, platform, pubkey) = check_device(d)?;
    let existing: Option<String> = db
        .query_row(
            "SELECT id FROM devices WHERE account_id = ?1 AND pubkey = ?2 AND revoked_at IS NULL",
            params![account_id, pubkey],
            |r| r.get(0),
        )
        .optional()?;
    if let Some(id) = existing {
        db.execute(
            "UPDATE devices SET name = ?1, platform = ?2, last_seen = ?3 WHERE id = ?4",
            params![name, platform, now, id],
        )?;
        return Ok(id);
    }
    let live: i64 = db.query_row(
        "SELECT COUNT(*) FROM devices WHERE account_id = ?1 AND revoked_at IS NULL",
        [account_id],
        |r| r.get(0),
    )?;
    if live >= 32 {
        return Err(ApiError::conflict(
            "device_limit",
            "32 devices are signed in; remove one first",
        ));
    }
    let id = new_id();
    db.execute(
        "INSERT INTO devices (id, account_id, name, platform, pubkey, created_at, last_seen) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?6)",
        params![id, account_id, name, platform, pubkey, now],
    )?;
    Ok(id)
}

pub fn key_state(db: &Connection, account_id: &str, device_id: &str) -> ApiResult<&'static str> {
    let (key_id, wrapped): (Option<String>, Option<String>) = db.query_row(
        "SELECT a.key_id, d.wrapped_key FROM devices d JOIN accounts a ON a.id = d.account_id WHERE d.id = ?1 AND a.id = ?2",
        params![device_id, account_id],
        |r| Ok((r.get(0)?, r.get(1)?)),
    )?;
    Ok(match (key_id, wrapped) {
        (_, Some(_)) => "granted",
        (None, None) => "bootstrap",
        (Some(_), None) => "pending",
    })
}

pub fn access_token(
    app: &AppState,
    db: &Connection,
    account_id: &str,
    device_id: &str,
    now: i64,
) -> ApiResult<String> {
    let sep: i64 = db.query_row(
        "SELECT session_epoch FROM devices WHERE id = ?1",
        [device_id],
        |r| r.get(0),
    )?;
    let mut st =
        db.prepare("SELECT id FROM profiles WHERE account_id = ?1 ORDER BY created_at, id")?;
    let prf = st
        .query_map([account_id], |r| r.get::<_, String>(0))?
        .collect::<Result<Vec<_>, _>>()?;
    Ok(app.jwt.sign(&Claims {
        iss: app.cfg.issuer.clone(),
        aud: AUDIENCE.into(),
        sub: account_id.into(),
        did: device_id.into(),
        sep,
        prf,
        iat: now,
        exp: now + app.cfg.access_ttl,
        jti: new_id(),
    }))
}

/// Issues an access token and a fresh refresh token for a device.
pub fn issue_tokens(
    app: &AppState,
    db: &Connection,
    account_id: &str,
    device_id: &str,
    now: i64,
) -> ApiResult<Value> {
    let refresh = random_token();
    db.execute(
        "INSERT INTO refresh_tokens (hash, account_id, device_id, created_at, expires_at) VALUES (?1, ?2, ?3, ?4, ?5)",
        params![token_hash(&refresh), account_id, device_id, now, now + app.cfg.refresh_ttl],
    )?;
    Ok(json!({
        "access_token": access_token(app, db, account_id, device_id, now)?,
        "token_type": "Bearer",
        "expires_in": app.cfg.access_ttl,
        "refresh_token": refresh,
        "refresh_expires_in": app.cfg.refresh_ttl,
        "account_id": account_id,
        "device_id": device_id,
        "key_state": key_state(db, account_id, device_id)?,
    }))
}

/// Ends every session of a device: refresh tokens deleted, access tokens rejected (epoch bump).
pub fn end_device_sessions(db: &Connection, device_id: &str) -> rusqlite::Result<()> {
    db.execute(
        "DELETE FROM refresh_tokens WHERE device_id = ?1",
        [device_id],
    )?;
    db.execute(
        "UPDATE devices SET session_epoch = session_epoch + 1 WHERE id = ?1",
        [device_id],
    )?;
    Ok(())
}

fn code_mac(app: &AppState, email: &str, code: &str) -> Vec<u8> {
    hmac_sha256(&app.code_key, format!("{email}|{code}").as_bytes()).to_vec()
}

#[derive(Deserialize)]
pub struct EmailStart {
    email: String,
}

pub async fn email_start(
    State(app): State<AppState>,
    ip: ClientIp,
    Json(b): Json<EmailStart>,
) -> ApiResult<(StatusCode, Json<Value>)> {
    limit(&app, "auth-ip", &ip.0, app.cfg.auth_ip_per_min, 60)?;
    let email = normalize_email(&b.email)
        .ok_or_else(|| ApiError::bad_request("email: not a valid address"))?;
    limit(&app, "email", &email, app.cfg.email_per_window, 15 * 60)?;
    let now = app.now();
    let code = random_code();
    let token = random_token();
    {
        let db = app.db();
        db.execute(
            "INSERT OR REPLACE INTO email_challenges (email, code_mac, token_hash, created_at, expires_at, attempts) VALUES (?1, ?2, ?3, ?4, ?5, 0)",
            params![email, code_mac(&app, &email, &code), token_hash(&token), now, now + app.cfg.email_code_ttl],
        )?;
    }
    let enc = |s: &str| utf8_percent_encode(s, NON_ALPHANUMERIC).to_string();
    let link = format!(
        "{}#email={}&token={}",
        app.cfg.link_base,
        enc(&email),
        token
    );
    let minutes = app.cfg.email_code_ttl / 60;
    let text = format!(
        "Your Napple sign-in code is {code}\n\nOr open this link on the device you are signing in on:\n{link}\n\nThe code and link work once and expire in {minutes} minutes.\nIf you did not ask to sign in to Napple, ignore this email.\n"
    );
    app.mailer
        .send(
            Mail {
                to: email.clone(),
                subject: format!("Napple sign-in code: {code}"),
                text,
            },
            now,
        )
        .await
        .map_err(|e| {
            crate::log::error(&format!("mailer: {e}"));
            ApiError::unavailable("mail_failed", "could not send the sign-in email, try again")
        })?;
    // Same answer whether or not an account exists for this address.
    Ok((
        StatusCode::ACCEPTED,
        Json(json!({"sent": true, "expires_in": app.cfg.email_code_ttl})),
    ))
}

#[derive(Deserialize)]
pub struct EmailFinish {
    email: String,
    #[serde(default)]
    code: Option<String>,
    /// The token from the magic link (instead of the code).
    #[serde(default)]
    token: Option<String>,
    device: DeviceIn,
}

pub async fn email_finish(
    State(app): State<AppState>,
    ip: ClientIp,
    Json(b): Json<EmailFinish>,
) -> ApiResult<Json<Value>> {
    limit(&app, "auth-ip", &ip.0, app.cfg.auth_ip_per_min, 60)?;
    let invalid =
        || ApiError::bad_request("invalid or expired code").with("error_code", "invalid_code");
    let email = normalize_email(&b.email).ok_or_else(invalid)?;
    // Across codes too: at most 20 guesses per address per hour (5 per code, 5 codes per 15 minutes).
    limit(&app, "email-finish", &email, 20, 3600)?;
    check_device(&b.device)?;
    let now = app.now();
    let db = app.db();
    let ch: Option<(Vec<u8>, Vec<u8>, i64, u32)> = db
        .query_row("SELECT code_mac, token_hash, expires_at, attempts FROM email_challenges WHERE email = ?1", [&email], |r| {
            Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?))
        })
        .optional()?;
    let Some((mac, thash, expires_at, attempts)) = ch else {
        return Err(invalid());
    };
    if now > expires_at || attempts >= app.cfg.email_code_attempts {
        db.execute("DELETE FROM email_challenges WHERE email = ?1", [&email])?;
        return Err(invalid());
    }
    let ok = match (&b.code, &b.token) {
        (Some(code), _) => ct_eq(&code_mac(&app, &email, code.trim()), &mac),
        (None, Some(token)) => ct_eq(&token_hash(token.trim()), &thash),
        (None, None) => false,
    };
    if !ok {
        db.execute(
            "UPDATE email_challenges SET attempts = attempts + 1 WHERE email = ?1",
            [&email],
        )?;
        return Err(invalid());
    }
    // Single use.
    db.execute("DELETE FROM email_challenges WHERE email = ?1", [&email])?;

    let existing: Option<String> = db
        .query_row("SELECT id FROM accounts WHERE email = ?1", [&email], |r| {
            r.get(0)
        })
        .optional()?;
    let (account_id, new_account) = match existing {
        Some(id) => (id, false),
        None => {
            let id = new_id();
            // Default name = the email's local part, unless it breaks the reserved-name rules (then "Player").
            let local: String = email
                .split('@')
                .next()
                .unwrap_or("")
                .chars()
                .take(32)
                .collect();
            let display =
                crate::names::check_name(&local, 32, &[]).unwrap_or_else(|_| "Player".into());
            db.execute("INSERT INTO accounts (id, email, display_name, created_at, last_seen) VALUES (?1, ?2, ?3, ?4, ?4)", params![id, email, display, now])?;
            // Every account starts with one player profile so the online identity exists from the first sign-in.
            db.execute(
                "INSERT INTO profiles (id, account_id, nickname, icon_id, created_at, updated_at) VALUES (?1, ?2, ?3, 0, ?4, ?4)",
                params![new_id(), id, display, now],
            )?;
            crate::log::info(&format!("account created {id}"));
            (id, true)
        }
    };
    let device_id = upsert_device(&db, &account_id, &b.device, now)?;
    let mut v = issue_tokens(&app, &db, &account_id, &device_id, now)?;
    v["new_account"] = json!(new_account);
    Ok(Json(v))
}

#[derive(Deserialize)]
pub struct RefreshIn {
    refresh_token: String,
}

pub async fn refresh(
    State(app): State<AppState>,
    ip: ClientIp,
    Json(b): Json<RefreshIn>,
) -> ApiResult<Json<Value>> {
    limit(&app, "auth-ip", &ip.0, app.cfg.auth_ip_per_min * 4, 60)?;
    let now = app.now();
    let db = app.db();
    let row: Option<(String, String, i64, Option<i64>)> = db
        .query_row(
            "SELECT account_id, device_id, expires_at, used_at FROM refresh_tokens WHERE hash = ?1",
            [token_hash(b.refresh_token.trim())],
            |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?)),
        )
        .optional()?;
    let Some((account_id, device_id, expires_at, used_at)) = row else {
        return Err(ApiError::unauthorized("invalid refresh token"));
    };
    if used_at.is_some() {
        // A rotated-out token came back: someone else holds a copy. End every session of that device.
        end_device_sessions(&db, &device_id)?;
        crate::log::warn(&format!(
            "refresh token reuse on device {device_id}: sessions ended"
        ));
        return Err(ApiError::new(
            StatusCode::UNAUTHORIZED,
            "refresh_reused",
            "refresh token already used; sign in again",
        ));
    }
    if now >= expires_at {
        return Err(ApiError::unauthorized("refresh token expired"));
    }
    let revoked: Option<i64> = db.query_row(
        "SELECT revoked_at FROM devices WHERE id = ?1",
        [&device_id],
        |r| r.get(0),
    )?;
    if revoked.is_some() {
        return Err(ApiError::unauthorized("device revoked"));
    }
    db.execute(
        "UPDATE refresh_tokens SET used_at = ?1 WHERE hash = ?2",
        params![now, token_hash(b.refresh_token.trim())],
    )?;
    // Drop older used tokens of this device beyond the last few (kept briefly for reuse detection).
    db.execute(
        "DELETE FROM refresh_tokens WHERE device_id = ?1 AND used_at IS NOT NULL AND used_at < ?2",
        params![device_id, now - app.cfg.refresh_ttl],
    )?;
    Ok(Json(issue_tokens(&app, &db, &account_id, &device_id, now)?))
}

pub async fn logout(State(app): State<AppState>, a: Authed) -> ApiResult<StatusCode> {
    end_device_sessions(&app.db(), &a.device_id)?;
    Ok(StatusCode::NO_CONTENT)
}
