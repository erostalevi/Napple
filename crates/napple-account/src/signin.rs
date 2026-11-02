//! Email + password accounts: signup, emailed-code verification, login, password reset, device registration, token
//! issue, refresh rotation, logout.

use crate::app::{AppState, Authed, ClientIp, limit};
use crate::error::{ApiError, ApiResult};
use crate::jwt::{AUDIENCE, Claims};
use crate::codes::{RESET, VERIFY, check_mail_limits, consume, invalid, send_code};
use crate::util::{
    b64_decode, b64url, clean_name, new_id, normalize_email, random_code, random_token, token_hash,
};
use axum::Json;
use axum::extract::State;
use axum::http::StatusCode;
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

/// Nicknames: 3-20 characters of `[A-Za-z0-9_.-]`, plus the reserved-name rules. Uniqueness is checked by the caller.
pub fn check_nickname(n: &str) -> ApiResult<String> {
    let n = n.trim();
    let len = n.chars().count();
    if !(3..=20).contains(&len)
        || !n.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '_' | '.' | '-'))
    {
        return Err(ApiError::bad_request("nickname: 3-20 characters, letters, digits, _ . -")
            .with("error_code", "bad_nickname"));
    }
    crate::names::check_name(n, 20, &[]).map_err(|e| {
        ApiError::bad_request(e.message()).with("error_code", "reserved_nickname")
    })
}

fn nickname_taken(db: &Connection, nick: &str, except_account: Option<&str>) -> rusqlite::Result<bool> {
    let ex = except_account.unwrap_or("");
    let taken: Option<i64> = db
        .query_row(
            "SELECT 1 FROM profiles WHERE lower(nickname) = lower(?1) AND account_id != ?2
             UNION SELECT 1 FROM accounts WHERE lower(pending_nickname) = lower(?1) AND id != ?2 AND verified_at IS NULL",
            params![nick, ex],
            |r| r.get(0),
        )
        .optional()?;
    Ok(taken.is_some())
}

async fn hash_blocking(password: String, m_kib: u32) -> ApiResult<String> {
    tokio::task::spawn_blocking(move || crate::password::hash(&password, m_kib))
        .await
        .map_err(ApiError::internal)?
        .map_err(ApiError::internal)
}

async fn verify_blocking(password: String, phc: String) -> bool {
    tokio::task::spawn_blocking(move || crate::password::verify(&password, &phc))
        .await
        .unwrap_or(false)
}

#[derive(Deserialize)]
pub struct Signup {
    email: String,
    password: String,
    nickname: String,
    #[serde(default)]
    age_ok: bool,
    #[serde(default)]
    terms_ok: bool,
    device: DeviceIn,
}

/// Creates an unverified account and mails a confirmation code. An address that already has a verified account
/// gets the same 202 and no mail (no enumeration).
pub async fn signup(
    State(app): State<AppState>,
    ip: ClientIp,
    Json(b): Json<Signup>,
) -> ApiResult<(StatusCode, Json<Value>)> {
    limit(&app, "auth-ip", &ip.0, app.cfg.auth_ip_per_min, 60)?;
    let email = normalize_email(&b.email)
        .ok_or_else(|| ApiError::bad_request("email: not a valid address"))?;
    crate::terms::require(b.age_ok, b.terms_ok)?;
    crate::password::check(&b.password, &email).map_err(|e| {
        ApiError::bad_request(e.message()).with("error_code", "weak_password")
    })?;
    let nick = check_nickname(&b.nickname)?;
    let (dname, dplatform, dpub) = check_device(&b.device)?;
    check_mail_limits(&app, &email, &ip.0)?;
    let hash = hash_blocking(b.password, app.cfg.argon_m_kib).await?;
    let now = app.now();
    let device = json!({"name": dname, "platform": dplatform, "pubkey": dpub}).to_string();
    let send = {
        let db = app.db();
        let existing: Option<(String, Option<i64>)> = db
            .query_row("SELECT id, verified_at FROM accounts WHERE email = ?1", [&email], |r| {
                Ok((r.get(0)?, r.get(1)?))
            })
            .optional()?;
        match existing {
            Some((_, Some(_))) => false,
            Some((id, None)) => {
                if nickname_taken(&db, &nick, Some(&id))? {
                    return Err(ApiError::conflict("nickname_taken", "that nickname is taken"));
                }
                db.execute(
                    "UPDATE accounts SET password_hash = ?2, pending_nickname = ?3, pending_device = ?4, display_name = ?3 WHERE id = ?1",
                    params![id, hash, nick, device],
                )?;
                crate::terms::record(&db, &id, now)?;
                true
            }
            None => {
                if nickname_taken(&db, &nick, None)? {
                    return Err(ApiError::conflict("nickname_taken", "that nickname is taken"));
                }
                let id = new_id();
                db.execute(
                    "INSERT INTO accounts (id, email, display_name, created_at, last_seen, password_hash, pending_nickname, pending_device) VALUES (?1, ?2, ?3, ?4, ?4, ?5, ?3, ?6)",
                    params![id, email, nick, now, hash, device],
                )?;
                crate::terms::record(&db, &id, now)?;
                crate::log::info(&format!("account created (unverified) {id}"));
                true
            }
        }
    };
    if send {
        send_code(&app, &email, VERIFY).await?;
    }
    Ok((
        StatusCode::ACCEPTED,
        Json(json!({"sent": true, "expires_in": app.cfg.email_code_ttl})),
    ))
}

#[derive(Deserialize)]
pub struct Verify {
    email: String,
    code: String,
}

/// Confirms the address with the emailed code: the account becomes usable, gets its first profile (the nickname from
/// signup) and the signup device signs in.
pub async fn verify(
    State(app): State<AppState>,
    ip: ClientIp,
    Json(b): Json<Verify>,
) -> ApiResult<Json<Value>> {
    limit(&app, "auth-ip", &ip.0, app.cfg.auth_ip_per_min, 60)?;
    let email = normalize_email(&b.email).ok_or_else(invalid)?;
    // Across codes too: at most 20 guesses per address per hour.
    limit(&app, "verify", &email, 20, 3600)?;
    consume(&app, &email, VERIFY, &b.code)?;
    let now = app.now();
    let db = app.db();
    let row: Option<(String, Option<i64>, Option<String>, Option<String>)> = db
        .query_row(
            "SELECT id, verified_at, pending_nickname, pending_device FROM accounts WHERE email = ?1",
            [&email],
            |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?)),
        )
        .optional()?;
    let Some((id, None, Some(nick), Some(device))) = row else {
        return Err(invalid());
    };
    let device: DeviceIn = serde_json::from_str::<Value>(&device)
        .ok()
        .and_then(|d| {
            Some(DeviceIn {
                name: d["name"].as_str()?.into(),
                platform: d["platform"].as_str()?.into(),
                pubkey: d["pubkey"].as_str()?.into(),
            })
        })
        .ok_or_else(|| ApiError::internal("pending device unreadable"))?;
    // The first profile is the online identity. Validate it like any profile nickname (reserved words, charset); if
    // someone took it meanwhile (or it no longer validates), add digits. All under the one DB lock, then
    // `profiles::insert` re-checks uniqueness case-insensitively.
    let mut name = crate::nicknames::validate(&db, &id, &nick).unwrap_or_else(|_| "Player".into());
    while nickname_taken(&db, &name, Some(&id))? || crate::nicknames::taken(&db, &name, None)? {
        let suffix = random_code();
        name = format!("{}{}", &nick[..nick.len().min(14)], &suffix[..4]);
    }
    db.execute(
        "UPDATE accounts SET verified_at = ?2, pending_nickname = NULL, pending_device = NULL WHERE id = ?1",
        params![id, now],
    )?;
    crate::profiles::insert(&db, &id, &name, 0, now)?;
    db.execute("UPDATE accounts SET display_name = ?2 WHERE id = ?1", params![id, name])?;
    let device_id = upsert_device(&db, &id, &device, now)?;
    let mut v = issue_tokens(&app, &db, &id, &device_id, now)?;
    v["new_account"] = json!(true);
    Ok(Json(v))
}

#[derive(Deserialize)]
pub struct Login {
    email: String,
    password: String,
    device: DeviceIn,
}

fn bad_credentials() -> ApiError {
    ApiError::new(StatusCode::UNAUTHORIZED, "invalid_credentials", "incorrect email or password")
}

/// Password sign-in. Unknown address, wrong password and a password-less legacy account all answer the same, and
/// the same time (a dummy hash is checked). 10 failures in 15 minutes lock the address for 15 minutes.
pub async fn login(
    State(app): State<AppState>,
    ip: ClientIp,
    Json(b): Json<Login>,
) -> ApiResult<Json<Value>> {
    limit(&app, "auth-ip", &ip.0, app.cfg.auth_ip_per_min, 60)?;
    let email = normalize_email(&b.email).ok_or_else(bad_credentials)?;
    check_device(&b.device)?;
    let now = app.now();
    let (row, locked) = {
        let db = app.db();
        let locked = crate::ratelimit::locked_for(&db, &email, now)?;
        let row: Option<(String, Option<String>, Option<i64>)> = db
            .query_row(
                "SELECT id, password_hash, verified_at FROM accounts WHERE email = ?1",
                [&email],
                |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
            )
            .optional()?;
        (row, locked)
    };
    if let Some(secs) = locked {
        let mut e = ApiError::too_many(secs);
        e.code = "locked";
        e.message = "too many failed sign-ins; try again later".into();
        return Err(e);
    }
    let (phc, known) = match &row {
        Some((_, Some(h), _)) => (h.clone(), true),
        _ => (crate::password::dummy_hash(app.cfg.argon_m_kib), false),
    };
    let ok = verify_blocking(b.password, phc).await && known;
    let Some((id, _, verified)) = row.filter(|_| ok) else {
        crate::ratelimit::fail(&app.db(), &email, app.cfg.login_fail_limit, now)?;
        return Err(bad_credentials());
    };
    crate::ratelimit::clear(&app.db(), &email)?;
    if verified.is_none() {
        // Right password, address not confirmed yet: send a fresh code.
        check_mail_limits(&app, &email, &ip.0)?;
        send_code(&app, &email, VERIFY).await?;
        return Err(ApiError::forbidden("confirm your email address: a new code was sent")
            .with("reason", "unverified"));
    }
    let db = app.db();
    let device_id = upsert_device(&db, &id, &b.device, now)?;
    let mut v = issue_tokens(&app, &db, &id, &device_id, now)?;
    v["new_account"] = json!(false);
    Ok(Json(v))
}

#[derive(Deserialize)]
pub struct ResetStart {
    email: String,
}

/// Always 202: nothing here says whether the address has an account.
pub async fn reset_start(
    State(app): State<AppState>,
    ip: ClientIp,
    Json(b): Json<ResetStart>,
) -> ApiResult<(StatusCode, Json<Value>)> {
    limit(&app, "auth-ip", &ip.0, app.cfg.auth_ip_per_min, 60)?;
    let email = normalize_email(&b.email)
        .ok_or_else(|| ApiError::bad_request("email: not a valid address"))?;
    check_mail_limits(&app, &email, &ip.0)?;
    let exists: bool = app
        .db()
        .query_row(
            "SELECT 1 FROM accounts WHERE email = ?1 AND verified_at IS NOT NULL",
            [&email],
            |_| Ok(()),
        )
        .optional()?
        .is_some();
    if exists {
        send_code(&app, &email, RESET).await?;
    }
    Ok((
        StatusCode::ACCEPTED,
        Json(json!({"sent": true, "expires_in": app.cfg.email_code_ttl})),
    ))
}

#[derive(Deserialize)]
pub struct ResetFinish {
    email: String,
    code: String,
    new_password: String,
}

/// Sets a new password with the emailed code, lifts any lockout and ends every session of the account.
pub async fn reset_finish(
    State(app): State<AppState>,
    ip: ClientIp,
    Json(b): Json<ResetFinish>,
) -> ApiResult<Json<Value>> {
    limit(&app, "auth-ip", &ip.0, app.cfg.auth_ip_per_min, 60)?;
    let email = normalize_email(&b.email).ok_or_else(invalid)?;
    limit(&app, "reset-finish", &email, 20, 3600)?;
    crate::password::check(&b.new_password, &email).map_err(|e| {
        ApiError::bad_request(e.message()).with("error_code", "weak_password")
    })?;
    consume(&app, &email, RESET, &b.code)?;
    let hash = hash_blocking(b.new_password, app.cfg.argon_m_kib).await?;
    let db = app.db();
    let id: Option<String> = db
        .query_row("SELECT id FROM accounts WHERE email = ?1", [&email], |r| r.get(0))
        .optional()?;
    let Some(id) = id else {
        return Err(invalid());
    };
    db.execute("UPDATE accounts SET password_hash = ?2 WHERE id = ?1", params![id, hash])?;
    crate::ratelimit::clear(&db, &email)?;
    let devices: Vec<String> = db
        .prepare("SELECT id FROM devices WHERE account_id = ?1")?
        .query_map([&id], |r| r.get(0))?
        .collect::<Result<_, _>>()?;
    for d in devices {
        end_device_sessions(&db, &d)?;
    }
    Ok(Json(json!({"reset": true})))
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
