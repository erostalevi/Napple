//! `/me`, player profiles, devices, and account deletion.

use crate::app::{AppState, Authed};
use crate::error::{ApiError, ApiResult};
use crate::plans::plan_json;
use crate::saves::{delete_versions, flush_deletions};
use crate::signin::end_device_sessions;
use crate::util::{clean_name, is_uuid, new_id, normalize_email};
use axum::Json;
use axum::extract::{Path, Query, State};
use axum::http::StatusCode;
use rusqlite::{OptionalExtension, params};
use serde::Deserialize;
use serde_json::{Value, json};
use std::collections::HashMap;

pub async fn me(State(app): State<AppState>, a: Authed) -> ApiResult<Json<Value>> {
    let now = app.now();
    let db = app.db();
    let (email, display, created, last_seen, key_id, has_recovery): (String, String, i64, i64, Option<String>, bool) = db.query_row(
        "SELECT email, display_name, created_at, last_seen, key_id, recovery_blob IS NOT NULL FROM accounts WHERE id = ?1",
        [&a.account_id],
        |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?, r.get(4)?, r.get(5)?)),
    )?;
    Ok(Json(json!({
        "account_id": a.account_id, "device_id": a.device_id, "email": email, "display_name": display,
        "created_at": created, "last_seen": last_seen, "key_id": key_id, "has_recovery": has_recovery,
        "key_state": crate::signin::key_state(&db, &a.account_id, &a.device_id)?,
        "plan": plan_json(&app, &db, &a.account_id, now)?,
    })))
}

#[derive(Deserialize)]
pub struct PatchMe {
    display_name: String,
}

pub async fn patch_me(
    State(app): State<AppState>,
    a: Authed,
    Json(b): Json<PatchMe>,
) -> ApiResult<Json<Value>> {
    let name = clean_name(&b.display_name, 32)
        .ok_or_else(|| ApiError::bad_request("display_name: 1-32 characters"))?;
    app.db().execute(
        "UPDATE accounts SET display_name = ?1 WHERE id = ?2",
        params![name, a.account_id],
    )?;
    Ok(Json(json!({"display_name": name})))
}

#[derive(Deserialize)]
pub struct DeleteMe {
    /// The account's email address, as a typed confirmation.
    confirm_email: String,
}

/// Deletes the account and everything in it: the Stripe customer (which cancels the subscription), every save
/// object, profiles, devices, passkeys, tokens. Irreversible.
pub async fn delete_me(
    State(app): State<AppState>,
    a: Authed,
    Json(b): Json<DeleteMe>,
) -> ApiResult<StatusCode> {
    let (email, customer, sub): (String, Option<String>, Option<String>) = app.db().query_row(
        "SELECT email, stripe_customer_id, sub_id FROM accounts WHERE id = ?1",
        [&a.account_id],
        |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
    )?;
    if normalize_email(&b.confirm_email).as_deref() != Some(email.as_str()) {
        return Err(
            ApiError::bad_request("confirm_email does not match this account")
                .with("error_code", "confirm_mismatch"),
        );
    }
    if customer.is_some() || sub.is_some() {
        crate::billing::cancel_everything(&app, customer.as_deref(), sub.as_deref()).await?;
    }
    let now = app.now();
    let keys = {
        let db = app.db();
        let keys = delete_versions(&db, "account_id = ?1", &[&a.account_id], now)?;
        db.execute("DELETE FROM accounts WHERE id = ?1", [&a.account_id])?;
        keys
    };
    crate::log::info(&format!(
        "account deleted {} ({} save objects queued)",
        a.account_id,
        keys.len()
    ));
    flush_deletions(&app, keys).await;
    Ok(StatusCode::NO_CONTENT)
}

fn profile_json(r: &rusqlite::Row) -> rusqlite::Result<Value> {
    Ok(
        json!({"profile_id": r.get::<_, String>(0)?, "nickname": r.get::<_, String>(1)?, "icon_id": r.get::<_, i64>(2)?,
              "created_at": r.get::<_, i64>(3)?, "updated_at": r.get::<_, i64>(4)?}),
    )
}

const PROFILE_COLS: &str = "id, nickname, icon_id, created_at, updated_at";

fn check_icon(i: i64) -> ApiResult<i64> {
    if (0..=65_535).contains(&i) {
        Ok(i)
    } else {
        Err(ApiError::bad_request("icon_id: 0-65535"))
    }
}

pub async fn list_profiles(State(app): State<AppState>, a: Authed) -> ApiResult<Json<Value>> {
    let db = app.db();
    let mut st = db.prepare(&format!(
        "SELECT {PROFILE_COLS} FROM profiles WHERE account_id = ?1 ORDER BY created_at, id"
    ))?;
    let v: Vec<Value> = st
        .query_map([&a.account_id], profile_json)?
        .collect::<Result<_, _>>()?;
    Ok(Json(
        json!({"profiles": v, "max_profiles": app.cfg.max_profiles}),
    ))
}

#[derive(Deserialize)]
pub struct NewProfile {
    nickname: String,
    #[serde(default)]
    icon_id: i64,
}

pub async fn create_profile(
    State(app): State<AppState>,
    a: Authed,
    Json(b): Json<NewProfile>,
) -> ApiResult<(StatusCode, Json<Value>)> {
    let nick = clean_name(&b.nickname, 32)
        .ok_or_else(|| ApiError::bad_request("nickname: 1-32 characters"))?;
    let icon = check_icon(b.icon_id)?;
    let now = app.now();
    let db = app.db();
    let n: i64 = db.query_row(
        "SELECT COUNT(*) FROM profiles WHERE account_id = ?1",
        [&a.account_id],
        |r| r.get(0),
    )?;
    if n >= app.cfg.max_profiles as i64 {
        return Err(ApiError::conflict(
            "profile_limit",
            format!("an account has at most {} profiles", app.cfg.max_profiles),
        ));
    }
    let id = new_id();
    db.execute("INSERT INTO profiles (id, account_id, nickname, icon_id, created_at, updated_at) VALUES (?1, ?2, ?3, ?4, ?5, ?5)", params![id, a.account_id, nick, icon, now])?;
    let v = db.query_row(
        &format!("SELECT {PROFILE_COLS} FROM profiles WHERE id = ?1"),
        [&id],
        profile_json,
    )?;
    Ok((StatusCode::CREATED, Json(v)))
}

pub async fn get_profile(
    State(app): State<AppState>,
    a: Authed,
    Path(id): Path<String>,
) -> ApiResult<Json<Value>> {
    let db = app.db();
    db.query_row(
        &format!("SELECT {PROFILE_COLS} FROM profiles WHERE id = ?1 AND account_id = ?2"),
        params![id, a.account_id],
        profile_json,
    )
    .optional()?
    .map(Json)
    .ok_or_else(|| ApiError::not_found("no such profile"))
}

#[derive(Deserialize)]
pub struct PatchProfile {
    #[serde(default)]
    nickname: Option<String>,
    #[serde(default)]
    icon_id: Option<i64>,
}

pub async fn patch_profile(
    State(app): State<AppState>,
    a: Authed,
    Path(id): Path<String>,
    Json(b): Json<PatchProfile>,
) -> ApiResult<Json<Value>> {
    let now = app.now();
    let db = app.db();
    let exists = db
        .query_row(
            "SELECT 1 FROM profiles WHERE id = ?1 AND account_id = ?2",
            params![id, a.account_id],
            |_| Ok(()),
        )
        .optional()?
        .is_some();
    if !exists {
        return Err(ApiError::not_found("no such profile"));
    }
    if let Some(n) = &b.nickname {
        let nick =
            clean_name(n, 32).ok_or_else(|| ApiError::bad_request("nickname: 1-32 characters"))?;
        db.execute(
            "UPDATE profiles SET nickname = ?1, updated_at = ?2 WHERE id = ?3",
            params![nick, now, id],
        )?;
    }
    if let Some(i) = b.icon_id {
        db.execute(
            "UPDATE profiles SET icon_id = ?1, updated_at = ?2 WHERE id = ?3",
            params![check_icon(i)?, now, id],
        )?;
    }
    Ok(Json(db.query_row(
        &format!("SELECT {PROFILE_COLS} FROM profiles WHERE id = ?1"),
        [&id],
        profile_json,
    )?))
}

/// Refuses while the profile has cloud saves unless `?delete_saves=true`.
pub async fn delete_profile(
    State(app): State<AppState>,
    a: Authed,
    Path(id): Path<String>,
    Query(q): Query<HashMap<String, String>>,
) -> ApiResult<StatusCode> {
    let now = app.now();
    let keys = {
        let db = app.db();
        if !is_uuid(&id)
            || db
                .query_row(
                    "SELECT 1 FROM profiles WHERE id = ?1 AND account_id = ?2",
                    params![id, a.account_id],
                    |_| Ok(()),
                )
                .optional()?
                .is_none()
        {
            return Err(ApiError::not_found("no such profile"));
        }
        let saves: i64 = db.query_row(
            "SELECT COUNT(*) FROM save_versions WHERE account_id = ?1 AND profile_id = ?2",
            params![a.account_id, id],
            |r| r.get(0),
        )?;
        if saves > 0 && q.get("delete_saves").map(String::as_str) != Some("true") {
            return Err(ApiError::conflict(
                "profile_has_saves",
                "this profile has cloud saves; pass ?delete_saves=true to delete them too",
            )
            .with("versions", saves));
        }
        let keys = delete_versions(
            &db,
            "account_id = ?1 AND profile_id = ?2",
            &[&a.account_id, &id],
            now,
        )?;
        db.execute("DELETE FROM profiles WHERE id = ?1", [&id])?;
        keys
    };
    flush_deletions(&app, keys).await;
    Ok(StatusCode::NO_CONTENT)
}

pub async fn list_devices(State(app): State<AppState>, a: Authed) -> ApiResult<Json<Value>> {
    let db = app.db();
    let mut st = db.prepare(
        "SELECT id, name, platform, pubkey, created_at, last_seen, wrapped_key IS NOT NULL FROM devices WHERE account_id = ?1 AND revoked_at IS NULL ORDER BY created_at",
    )?;
    let v: Vec<Value> = st
        .query_map([&a.account_id], |r| {
            let id: String = r.get(0)?;
            Ok(json!({"device_id": id, "name": r.get::<_, String>(1)?, "platform": r.get::<_, String>(2)?, "pubkey": r.get::<_, String>(3)?,
                      "created_at": r.get::<_, i64>(4)?, "last_seen": r.get::<_, i64>(5)?,
                      "key_state": if r.get::<_, bool>(6)? { "granted" } else { "pending" }, "current": id == a.device_id}))
        })?
        .collect::<Result<_, _>>()?;
    Ok(Json(json!({"devices": v})))
}

/// Signs the device out everywhere and drops its wrapped save key. (It may still have the key in memory: rotating
/// the account key after a lost device is a client-side re-encryption, see the guide's threat model.)
pub async fn revoke_device(
    State(app): State<AppState>,
    a: Authed,
    Path(id): Path<String>,
) -> ApiResult<StatusCode> {
    let now = app.now();
    let db = app.db();
    let n = db.execute(
        "UPDATE devices SET revoked_at = ?1, wrapped_key = NULL WHERE id = ?2 AND account_id = ?3 AND revoked_at IS NULL",
        params![now, id, a.account_id],
    )?;
    if n == 0 {
        return Err(ApiError::not_found("no such device"));
    }
    end_device_sessions(&db, &id)?;
    Ok(StatusCode::NO_CONTENT)
}
