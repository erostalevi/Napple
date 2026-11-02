//! Cloud-save version metadata. Per (title, profile) a linear history: create a pending version against the current
//! head (`base_version`), PUT the ciphertext to the presigned URL, commit. A stale base gets 409 with the head.
//! The server stores ciphertext only (size + SHA-256 pinned at create, checked by the store on PUT and again at commit).

use crate::app::{App, AppState, Authed};
use crate::blob::SAVE_CONTENT_TYPE;
use crate::error::{ApiError, ApiResult};
use crate::util::{is_uuid, new_id, normalize_title_id};
use axum::Json;
use axum::body::Bytes;
use axum::extract::{Path, Query, State};
use axum::http::{HeaderMap, StatusCode, header};
use axum::response::{IntoResponse, Response};
use rusqlite::{Connection, OptionalExtension, params};
use serde::Deserialize;
use serde_json::{Value, json};
use std::collections::HashMap;

fn title_of(s: &str) -> ApiResult<String> {
    normalize_title_id(s).ok_or_else(|| ApiError::bad_request("title id: 16 hex digits"))
}

fn own_profile(db: &Connection, account_id: &str, profile: &str) -> ApiResult<()> {
    let ok = is_uuid(profile)
        && db
            .query_row(
                "SELECT 1 FROM profiles WHERE id = ?1 AND account_id = ?2",
                params![profile, account_id],
                |_| Ok(()),
            )
            .optional()?
            .is_some();
    if ok {
        Ok(())
    } else {
        Err(ApiError::not_found("no such profile"))
    }
}

fn head(
    db: &Connection,
    account_id: &str,
    title: &str,
    profile: &str,
) -> rusqlite::Result<Option<Value>> {
    db.query_row(
        "SELECT version, size, sha256, committed_at, device_id, device_name FROM save_versions
         WHERE account_id = ?1 AND title_id = ?2 AND profile_id = ?3 AND state = 'committed' ORDER BY version DESC LIMIT 1",
        params![account_id, title, profile],
        version_json,
    )
    .optional()
}

fn version_json(r: &rusqlite::Row) -> rusqlite::Result<Value> {
    Ok(json!({
        "version": r.get::<_, i64>(0)?, "size": r.get::<_, i64>(1)?, "sha256_ciphertext": r.get::<_, String>(2)?,
        "committed_at": r.get::<_, Option<i64>>(3)?, "created_at": r.get::<_, Option<i64>>(3)?, "device_id": r.get::<_, String>(4)?, "device_name": r.get::<_, String>(5)?,
    }))
}

pub fn queue_delete(db: &Connection, key: &str, now: i64) -> rusqlite::Result<()> {
    db.execute(
        "INSERT OR IGNORE INTO blob_deletions (object_key, queued_at) VALUES (?1, ?2)",
        params![key, now],
    )?;
    Ok(())
}

/// Deletes queued objects now (best effort; whatever fails stays queued for the retention job).
pub async fn flush_deletions(app: &App, keys: Vec<String>) {
    let now = app.now();
    for k in keys {
        match app.blobs.delete(&k, now).await {
            Ok(()) => {
                let _ = app
                    .db()
                    .execute("DELETE FROM blob_deletions WHERE object_key = ?1", [&k]);
            }
            Err(e) => crate::log::warn(&format!("blob delete deferred: {e}")),
        }
    }
}

/// Queues every object of the matching versions for deletion and removes the rows. Returns the keys.
pub fn delete_versions(
    db: &Connection,
    where_sql: &str,
    args: &[&dyn rusqlite::ToSql],
    now: i64,
) -> rusqlite::Result<Vec<String>> {
    let mut st = db.prepare(&format!(
        "SELECT object_key FROM save_versions WHERE {where_sql}"
    ))?;
    let keys: Vec<String> = st
        .query_map(args, |r| r.get(0))?
        .collect::<Result<_, _>>()?;
    for k in &keys {
        queue_delete(db, k, now)?;
    }
    db.execute(
        &format!("DELETE FROM save_versions WHERE {where_sql}"),
        args,
    )?;
    Ok(keys)
}

/// Storage used by an account: committed versions plus uploads still in flight.
pub fn usage(app: &App, db: &Connection, account_id: &str, now: i64) -> rusqlite::Result<u64> {
    db.query_row(
        "SELECT COALESCE(SUM(size), 0) FROM save_versions WHERE account_id = ?1 AND (state = 'committed' OR created_at > ?2)",
        params![account_id, now - app.cfg.pending_upload_ttl],
        |r| r.get::<_, i64>(0),
    )
    .map(|v| v as u64)
}

pub async fn list(State(app): State<AppState>, a: Authed) -> ApiResult<Json<Value>> {
    let db = app.db();
    let mut st = db.prepare(
        "SELECT s.title_id, s.profile_id, s.version, s.size, s.committed_at, s.device_id, s.device_name
         FROM save_versions s
         WHERE s.account_id = ?1 AND s.state = 'committed' AND s.version = (
            SELECT MAX(version) FROM save_versions x WHERE x.account_id = s.account_id AND x.title_id = s.title_id AND x.profile_id = s.profile_id AND x.state = 'committed')
         ORDER BY s.committed_at DESC",
    )?;
    let saves: Vec<Value> = st
        .query_map([&a.account_id], |r| {
            Ok(json!({
                "title_id": r.get::<_, String>(0)?, "profile_id": r.get::<_, String>(1)?, "latest_version": r.get::<_, i64>(2)?,
                "size": r.get::<_, i64>(3)?, "updated_at": r.get::<_, i64>(4)?, "device_id": r.get::<_, String>(5)?, "device_name": r.get::<_, String>(6)?,
            }))
        })?
        .collect::<Result<_, _>>()?;
    Ok(Json(json!({"saves": saves})))
}

pub async fn delete_title(
    State(app): State<AppState>,
    a: Authed,
    Path(title): Path<String>,
) -> ApiResult<StatusCode> {
    let t = title_of(&title)?;
    let now = app.now();
    let keys = delete_versions(
        &app.db(),
        "account_id = ?1 AND title_id = ?2",
        &[&a.account_id, &t],
        now,
    )?;
    flush_deletions(&app, keys).await;
    Ok(StatusCode::NO_CONTENT)
}

pub async fn versions(
    State(app): State<AppState>,
    a: Authed,
    Path((title, profile)): Path<(String, String)>,
) -> ApiResult<Json<Value>> {
    let t = title_of(&title)?;
    let db = app.db();
    own_profile(&db, &a.account_id, &profile)?;
    let mut st = db.prepare(
        "SELECT version, size, sha256, committed_at, device_id, device_name FROM save_versions
         WHERE account_id = ?1 AND title_id = ?2 AND profile_id = ?3 AND state = 'committed' ORDER BY version DESC",
    )?;
    let v: Vec<Value> = st
        .query_map(params![a.account_id, t, profile], version_json)?
        .collect::<Result<_, _>>()?;
    Ok(Json(
        json!({"title_id": t, "profile_id": profile, "versions": v}),
    ))
}

#[derive(Deserialize)]
pub struct CreateVersion {
    #[serde(default)]
    base_version: Option<i64>,
    size: u64,
    sha256_ciphertext: String,
}

pub async fn create_version(
    State(app): State<AppState>,
    a: Authed,
    Path((title, profile)): Path<(String, String)>,
    Json(b): Json<CreateVersion>,
) -> ApiResult<Response> {
    let t = title_of(&title)?;
    let sha = b.sha256_ciphertext.trim().to_ascii_lowercase();
    if sha.len() != 64 || !sha.bytes().all(|c| c.is_ascii_hexdigit()) {
        return Err(ApiError::bad_request("sha256_ciphertext: 64 hex digits"));
    }
    if b.size == 0 {
        return Err(ApiError::bad_request("size: at least 1 byte"));
    }
    if b.size > app.cfg.max_version_bytes {
        return Err(ApiError::new(
            StatusCode::PAYLOAD_TOO_LARGE,
            "too_large",
            format!(
                "a save version is 1 byte to {} bytes",
                app.cfg.max_version_bytes
            ),
        )
        .with("reason", "too_large")
        .with("max_version_bytes", app.cfg.max_version_bytes));
    }
    let now = app.now();
    let (id, version, key, stale) = {
        let db = app.db();
        own_profile(&db, &a.account_id, &profile)?;
        let current = head(&db, &a.account_id, &t, &profile)?;
        let head_v = current
            .as_ref()
            .and_then(|h| h["version"].as_i64())
            .unwrap_or(0);
        if b.base_version.unwrap_or(0) != head_v {
            return Err(
                ApiError::conflict("version_conflict", "the cloud has a newer version")
                    .with("head", current.unwrap_or(Value::Null)),
            );
        }
        // Replace this device's earlier unfinished upload for the same save.
        let stale = delete_versions(
            &db,
            "account_id = ?1 AND title_id = ?2 AND profile_id = ?3 AND device_id = ?4 AND state = 'pending'",
            &[&a.account_id, &t, &profile, &a.device_id],
            now,
        )?;
        let quota = app.cfg.save_quota_bytes;
        let used = usage(&app, &db, &a.account_id, now)?;
        if used + b.size > quota {
            return Err(ApiError::new(
                StatusCode::PAYLOAD_TOO_LARGE,
                "quota_exceeded",
                "cloud save storage for this account is full",
            )
            .with("reason", "quota")
            .with("quota", quota)
            .with("used", used));
        }
        let device_name: String = db.query_row(
            "SELECT name FROM devices WHERE id = ?1",
            [&a.device_id],
            |r| r.get(0),
        )?;
        let id = new_id();
        let key = format!("saves/{}/{}/{}/{}", a.account_id, t, profile, id);
        db.execute(
            "INSERT INTO save_versions (id, account_id, title_id, profile_id, version, size, sha256, state, device_id, device_name, object_key, created_at)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, 'pending', ?8, ?9, ?10, ?11)",
            params![id, a.account_id, t, profile, head_v + 1, b.size as i64, sha, a.device_id, device_name, key, now],
        )?;
        (id, head_v + 1, key, stale)
    };
    flush_deletions(&app, stale).await;
    let up = app
        .blobs
        .presign_put(&key, b.size, &sha, now, app.cfg.upload_url_ttl)
        .await
        .map_err(ApiError::internal)?;
    Ok((
        StatusCode::CREATED,
        Json(json!({"version": version, "upload_id": id, "upload_url": up.url, "upload_method": up.method, "upload_headers": up.headers, "expires_at": up.expires_at})),
    )
        .into_response())
}

#[derive(Deserialize, Default)]
struct CommitIn {
    #[serde(default)]
    upload_id: Option<String>,
}

pub async fn commit_version(
    State(app): State<AppState>,
    a: Authed,
    Path((title, profile, v)): Path<(String, String, i64)>,
    body: Bytes,
) -> ApiResult<Json<Value>> {
    let t = title_of(&title)?;
    let ci: CommitIn = if body.is_empty() {
        CommitIn::default()
    } else {
        serde_json::from_slice(&body).map_err(|_| ApiError::bad_request("body: {upload_id}"))?
    };
    let now = app.now();
    let (id, key, size) = {
        let db = app.db();
        own_profile(&db, &a.account_id, &profile)?;
        let row: Option<(String, String, i64, i64)> = db
            .query_row(
                "SELECT id, object_key, size, created_at FROM save_versions WHERE account_id = ?1 AND title_id = ?2 AND profile_id = ?3 AND version = ?4
                 AND state = 'pending' AND (id = ?5 OR (?5 IS NULL AND device_id = ?6)) ORDER BY created_at DESC LIMIT 1",
                params![a.account_id, t, profile, v, ci.upload_id, a.device_id],
                |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?)),
            )
            .optional()?;
        let Some((id, key, size, created)) = row else {
            return Err(ApiError::not_found("no pending upload for this version"));
        };
        if now > created + app.cfg.pending_upload_ttl {
            return Err(ApiError::new(
                StatusCode::GONE,
                "upload_expired",
                "the upload expired; create the version again",
            ));
        }
        if let Some(h) = head(&db, &a.account_id, &t, &profile)?
            && h["version"].as_i64() != Some(v - 1)
        {
            return Err(
                ApiError::conflict("version_conflict", "another device committed first")
                    .with("head", h),
            );
        }
        (id, key, size)
    };
    match app
        .blobs
        .head(&key, now)
        .await
        .map_err(ApiError::internal)?
    {
        Some(n) if n == size as u64 => {}
        Some(_) => {
            return Err(ApiError::bad_request("uploaded object has the wrong size")
                .with("error_code", "upload_mismatch"));
        }
        None => {
            return Err(
                ApiError::bad_request("nothing uploaded yet").with("error_code", "upload_missing")
            );
        }
    }
    let pruned = {
        let db = app.db();
        if let Some(h) = head(&db, &a.account_id, &t, &profile)?
            && h["version"].as_i64() != Some(v - 1)
        {
            return Err(
                ApiError::conflict("version_conflict", "another device committed first")
                    .with("head", h),
            );
        }
        db.execute(
            "UPDATE save_versions SET state = 'committed', committed_at = ?1 WHERE id = ?2",
            params![now, id],
        )
        .map_err(|_| ApiError::conflict("version_conflict", "another device committed first"))?;
        let keep = app.cfg.save_versions as i64;
        delete_versions(
            &db,
            "account_id = ?1 AND title_id = ?2 AND profile_id = ?3 AND state = 'committed' AND version <= ?4",
            &[&a.account_id, &t, &profile, &(v - keep)],
            now,
        )?
    };
    flush_deletions(&app, pruned).await;
    let h = head(&app.db(), &a.account_id, &t, &profile)?;
    Ok(Json(h.unwrap_or(Value::Null)))
}

/// Downloads always work, read-only titles included (never hold data hostage).
pub async fn get_version(
    State(app): State<AppState>,
    a: Authed,
    Path((title, profile, v)): Path<(String, String, i64)>,
) -> ApiResult<Json<Value>> {
    let t = title_of(&title)?;
    let now = app.now();
    let (mut info, key) = {
        let db = app.db();
        own_profile(&db, &a.account_id, &profile)?;
        db.query_row(
            "SELECT version, size, sha256, committed_at, device_id, device_name, object_key FROM save_versions
             WHERE account_id = ?1 AND title_id = ?2 AND profile_id = ?3 AND version = ?4 AND state = 'committed'",
            params![a.account_id, t, profile, v],
            |r| Ok((version_json(r)?, r.get::<_, String>(6)?)),
        )
        .optional()?
        .ok_or_else(|| ApiError::not_found("no such version"))?
    };
    let dl = app
        .blobs
        .presign_get(&key, now, app.cfg.upload_url_ttl)
        .await
        .map_err(ApiError::internal)?;
    info["url"] = json!(dl.url);
    info["download_url"] = json!(dl.url);
    info["expires_at"] = json!(dl.expires_at);
    Ok(Json(info))
}

// ---- dev blob store endpoints (only routed with the filesystem backend) ----

fn q(m: &HashMap<String, String>, k: &str) -> String {
    m.get(k).cloned().unwrap_or_default()
}

pub async fn dev_blob_put(
    State(app): State<AppState>,
    Path(key): Path<String>,
    Query(qs): Query<HashMap<String, String>>,
    headers: HeaderMap,
    body: Bytes,
) -> Response {
    let Some(store) = app.dev_blobs.clone() else {
        return StatusCode::NOT_FOUND.into_response();
    };
    let len: u64 = q(&qs, "len").parse().unwrap_or(u64::MAX);
    let sha = q(&qs, "sha");
    let exp: i64 = q(&qs, "exp").parse().unwrap_or(0);
    if q(&qs, "op") != "put" || !store.check("put", &key, len, &sha, exp, &q(&qs, "sig"), app.now())
    {
        return (StatusCode::FORBIDDEN, "bad or expired signature").into_response();
    }
    if headers
        .get(header::CONTENT_TYPE)
        .and_then(|v| v.to_str().ok())
        != Some(SAVE_CONTENT_TYPE)
    {
        return (
            StatusCode::BAD_REQUEST,
            "content-type must be application/octet-stream",
        )
            .into_response();
    }
    match store.store(&key, len, &sha, &body) {
        Ok(()) => StatusCode::OK.into_response(),
        Err(e) => (StatusCode::BAD_REQUEST, e).into_response(),
    }
}

pub async fn dev_blob_get(
    State(app): State<AppState>,
    Path(key): Path<String>,
    Query(qs): Query<HashMap<String, String>>,
) -> Response {
    let Some(store) = app.dev_blobs.clone() else {
        return StatusCode::NOT_FOUND.into_response();
    };
    let exp: i64 = q(&qs, "exp").parse().unwrap_or(0);
    if q(&qs, "op") != "get" || !store.check("get", &key, 0, "", exp, &q(&qs, "sig"), app.now()) {
        return (StatusCode::FORBIDDEN, "bad or expired signature").into_response();
    }
    match store.path_of(&key).and_then(|p| std::fs::read(p).ok()) {
        Some(b) => ([(header::CONTENT_TYPE, SAVE_CONTENT_TYPE)], b).into_response(),
        None => StatusCode::NOT_FOUND.into_response(),
    }
}
