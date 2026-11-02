//! End-to-end save-key exchange. The account save key (random 256-bit) never reaches the server in the clear:
//! devices exchange it HPKE-wrapped (X25519 + HKDF-SHA256 + AES-256-GCM) to each device's X25519 public key, and the
//! recovery blob is the key wrapped under an Argon2id-derived key from a 24-word code the server never sees.
//! The server stores opaque blobs, checks who may write them, and, when clients send a key fingerprint (`key_id`), pins it so two
//! devices bootstrapping at the same time cannot end up with different account keys.

use crate::app::{AppState, Authed};
use crate::error::{ApiError, ApiResult};
use crate::util::{b64_decode, b64url};
use axum::Json;
use axum::extract::State;
use rusqlite::{OptionalExtension, params};
use serde::Deserialize;
use serde_json::{Value, json};

const MAX_WRAPPED: usize = 1024;
const MAX_RECOVERY: usize = 4096;

fn check_key_id(k: &str) -> ApiResult<()> {
    if k.is_empty()
        || k.len() > 64
        || !k
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
    {
        return Err(ApiError::bad_request(
            "key_id: 1-64 characters [A-Za-z0-9_-]",
        ));
    }
    Ok(())
}

fn blob(s: &str, max: usize, what: &str) -> ApiResult<String> {
    let b = b64_decode(s)
        .filter(|b| b.len() >= 32 && b.len() <= max)
        .ok_or_else(|| ApiError::bad_request(format!("{what}: base64url, 32..{max} bytes")))?;
    Ok(b64url(&b))
}

/// New devices of this account still waiting for the save key (excluding the caller).
pub async fn requests(State(app): State<AppState>, a: Authed) -> ApiResult<Json<Value>> {
    let db = app.db();
    let mut st = db.prepare(
        "SELECT id, name, platform, pubkey, created_at FROM devices WHERE account_id = ?1 AND revoked_at IS NULL AND wrapped_key IS NULL AND id != ?2 ORDER BY created_at",
    )?;
    let v: Vec<Value> = st
        .query_map(params![a.account_id, a.device_id], |r| {
            Ok(json!({"device_id": r.get::<_, String>(0)?, "name": r.get::<_, String>(1)?, "platform": r.get::<_, String>(2)?,
                      "pubkey": r.get::<_, String>(3)?, "created_at": r.get::<_, i64>(4)?}))
        })?
        .collect::<Result<_, _>>()?;
    Ok(Json(json!({"requests": v})))
}

#[derive(Deserialize)]
pub struct Grant {
    device_id: String,
    wrapped_account_key: String,
    /// Optional fingerprint of the account key; once one grant carried it, later grants must match.
    #[serde(default)]
    key_id: Option<String>,
}

/// Stores the account key wrapped to `device_id`'s public key. Granting another device requires the caller to hold
/// the key; a device may always store its own (first-device bootstrap, or after unwrapping the recovery blob).
pub async fn grant(
    State(app): State<AppState>,
    a: Authed,
    Json(b): Json<Grant>,
) -> ApiResult<Json<Value>> {
    if let Some(k) = &b.key_id {
        check_key_id(k)?;
    }
    let wrapped = blob(&b.wrapped_account_key, MAX_WRAPPED, "wrapped_account_key")?;
    let now = app.now();
    let db = app.db();
    let target: Option<Option<i64>> = db
        .query_row(
            "SELECT revoked_at FROM devices WHERE id = ?1 AND account_id = ?2",
            params![b.device_id, a.account_id],
            |r| r.get(0),
        )
        .optional()?;
    if !matches!(target, Some(None)) {
        return Err(ApiError::not_found("no such device"));
    }
    if b.device_id != a.device_id {
        let holds: Option<String> = db.query_row(
            "SELECT wrapped_key FROM devices WHERE id = ?1",
            [&a.device_id],
            |r| r.get(0),
        )?;
        if holds.is_none() {
            return Err(ApiError::forbidden(
                "only a device that holds the save key can approve another device",
            )
            .with("error_code", "not_key_holder"));
        }
    }
    let current: Option<String> = db.query_row(
        "SELECT key_id FROM accounts WHERE id = ?1",
        [&a.account_id],
        |r| r.get(0),
    )?;
    match (current, &b.key_id) {
        (_, None) => {}
        (None, Some(new)) => {
            db.execute(
                "UPDATE accounts SET key_id = ?1 WHERE id = ?2 AND key_id IS NULL",
                params![new, a.account_id],
            )?;
        }
        (Some(k), Some(new)) if &k != new => {
            return Err(ApiError::conflict(
                "key_mismatch",
                "this account already has a different save key",
            )
            .with("key_id", k));
        }
        (Some(_), Some(_)) => {}
    }
    db.execute(
        "UPDATE devices SET wrapped_key = ?1, key_granted_by = ?2, key_granted_at = ?3 WHERE id = ?4",
        params![wrapped, a.device_id, now, b.device_id],
    )?;
    Ok(Json(
        json!({"device_id": b.device_id, "key_id": b.key_id, "granted_at": now}),
    ))
}

pub async fn mine(State(app): State<AppState>, a: Authed) -> ApiResult<Json<Value>> {
    let db = app.db();
    let (key_id, wrapped, by, at): (Option<String>, Option<String>, Option<String>, Option<i64>) = db.query_row(
        "SELECT a.key_id, d.wrapped_key, d.key_granted_by, d.key_granted_at FROM devices d JOIN accounts a ON a.id = d.account_id WHERE d.id = ?1",
        [&a.device_id],
        |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?)),
    )?;
    match wrapped {
        Some(w) => Ok(Json(
            json!({"key_id": key_id, "wrapped_account_key": w, "granted_by": by, "granted_at": at}),
        )),
        None => Err(ApiError::not_found("this device has not been approved yet")
            .with("error_code", "key_pending")
            .with(
                "key_state",
                if key_id.is_some() {
                    "pending"
                } else {
                    "bootstrap"
                },
            )),
    }
}

/// The recovery record is the client's own JSON object (e.g. `{kdf: "argon2id", m_kib, t, p, salt, wrapped}`), stored and
/// returned verbatim (≤ 4 KiB). Only a device holding the key may set it; an optional `key_id` must match the account's.
pub async fn put_recovery(
    State(app): State<AppState>,
    a: Authed,
    Json(b): Json<Value>,
) -> ApiResult<Json<Value>> {
    if !b.is_object() || b.to_string().len() > MAX_RECOVERY {
        return Err(ApiError::bad_request(format!(
            "recovery record: a JSON object of at most {MAX_RECOVERY} bytes"
        )));
    }
    let now = app.now();
    let db = app.db();
    let (key_id, holds): (Option<String>, Option<String>) = db.query_row(
        "SELECT a.key_id, d.wrapped_key FROM devices d JOIN accounts a ON a.id = d.account_id WHERE d.id = ?1",
        [&a.device_id],
        |r| Ok((r.get(0)?, r.get(1)?)),
    )?;
    if holds.is_none() {
        return Err(ApiError::forbidden(
            "only a device that holds the save key can set the recovery record",
        )
        .with("error_code", "not_key_holder"));
    }
    if let (Some(k), Some(given)) = (&key_id, b["key_id"].as_str())
        && k != given
    {
        return Err(ApiError::conflict(
            "key_mismatch",
            "recovery record is for a different save key",
        )
        .with("key_id", k.clone()));
    }
    db.execute(
        "UPDATE accounts SET recovery_blob = ?1, recovery_updated_at = ?2 WHERE id = ?3",
        params![b.to_string(), now, a.account_id],
    )?;
    Ok(Json(b))
}

pub async fn get_recovery(State(app): State<AppState>, a: Authed) -> ApiResult<Json<Value>> {
    let r: Option<String> = app.db().query_row(
        "SELECT recovery_blob FROM accounts WHERE id = ?1",
        [&a.account_id],
        |r| r.get(0),
    )?;
    match r.and_then(|s| serde_json::from_str::<Value>(&s).ok()) {
        Some(v) => Ok(Json(v)),
        None => Err(ApiError::not_found("no recovery record set")),
    }
}
