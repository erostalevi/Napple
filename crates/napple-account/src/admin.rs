//! Per-account grants (`owner_napple`, `reserved_prefix`, `admin`), set only by an admin.
//! Admins: accounts whose verified email is in `NAPPLE_ADMIN_EMAILS` (bootstrap), plus accounts with the `admin` grant.
//! Every account email is verified (accounts are only created by the email code / link flow).
//! Same operations from the server binary: `napple-account admin grant|revoke|list <email> [grant]`.

use crate::app::{App, AppState, Authed};
use crate::error::{ApiError, ApiResult};
use crate::names::{GRANT_ADMIN, GRANTS};
use crate::util::normalize_email;
use axum::Json;
use axum::extract::{Query, State};
use rusqlite::{Connection, OptionalExtension, params};
use serde::Deserialize;
use serde_json::{Value, json};
use std::collections::HashMap;

pub fn grants_of(db: &Connection, account_id: &str) -> rusqlite::Result<Vec<String>> {
    let mut st = db.prepare("SELECT name FROM grants WHERE account_id = ?1 ORDER BY name")?;
    st.query_map([account_id], |r| r.get(0))?.collect()
}

pub fn is_admin(app: &App, db: &Connection, account_id: &str) -> rusqlite::Result<bool> {
    let email: Option<String> = db
        .query_row(
            "SELECT email FROM accounts WHERE id = ?1",
            [account_id],
            |r| r.get(0),
        )
        .optional()?;
    if email.is_some_and(|e| app.cfg.admin_emails.contains(&e)) {
        return Ok(true);
    }
    Ok(grants_of(db, account_id)?.iter().any(|g| g == GRANT_ADMIN))
}

/// `account_id` or `email` -> the account id.
pub fn resolve(
    db: &Connection,
    account_id: Option<&str>,
    email: Option<&str>,
) -> rusqlite::Result<Option<String>> {
    if let Some(id) = account_id {
        return db
            .query_row("SELECT id FROM accounts WHERE id = ?1", [id], |r| r.get(0))
            .optional();
    }
    match email.and_then(normalize_email) {
        Some(e) => db
            .query_row("SELECT id FROM accounts WHERE email = ?1", [e], |r| {
                r.get(0)
            })
            .optional(),
        None => Ok(None),
    }
}

/// Adds (`on`) or removes a grant. Names chosen earlier stay until the next rename, which is checked again.
pub fn set_grant(
    db: &Connection,
    account_id: &str,
    grant: &str,
    on: bool,
    by: Option<&str>,
    now: i64,
) -> Result<Vec<String>, String> {
    if !GRANTS.contains(&grant) {
        return Err(format!(
            "unknown grant {grant} (one of {})",
            GRANTS.join(", ")
        ));
    }
    let r = if on {
        db.execute("INSERT OR IGNORE INTO grants (account_id, name, granted_by, created_at) VALUES (?1, ?2, ?3, ?4)", params![account_id, grant, by, now])
    } else {
        db.execute(
            "DELETE FROM grants WHERE account_id = ?1 AND name = ?2",
            params![account_id, grant],
        )
    };
    r.map_err(|e| e.to_string())?;
    grants_of(db, account_id).map_err(|e| e.to_string())
}

fn require_admin(app: &App, db: &Connection, a: &Authed) -> ApiResult<()> {
    if is_admin(app, db, &a.account_id)? {
        Ok(())
    } else {
        Err(ApiError::forbidden("admins only"))
    }
}

#[derive(Deserialize)]
pub struct GrantIn {
    #[serde(default)]
    account_id: Option<String>,
    #[serde(default)]
    email: Option<String>,
    grant: String,
}

async fn change(app: AppState, a: Authed, b: GrantIn, on: bool) -> ApiResult<Json<Value>> {
    let now = app.now();
    let db = app.db();
    require_admin(&app, &db, &a)?;
    let target = resolve(&db, b.account_id.as_deref(), b.email.as_deref())?
        .ok_or_else(|| ApiError::not_found("no such account (it must sign in once first)"))?;
    let grants = set_grant(&db, &target, &b.grant, on, Some(&a.account_id), now)
        .map_err(ApiError::bad_request)?;
    crate::log::info(&format!(
        "admin {} {} grant {} on {target}",
        a.account_id,
        if on { "added" } else { "removed" },
        b.grant
    ));
    Ok(Json(json!({"account_id": target, "grants": grants})))
}

pub async fn add(
    State(app): State<AppState>,
    a: Authed,
    Json(b): Json<GrantIn>,
) -> ApiResult<Json<Value>> {
    change(app, a, b, true).await
}

pub async fn remove(
    State(app): State<AppState>,
    a: Authed,
    Json(b): Json<GrantIn>,
) -> ApiResult<Json<Value>> {
    change(app, a, b, false).await
}

pub async fn list(
    State(app): State<AppState>,
    a: Authed,
    Query(q): Query<HashMap<String, String>>,
) -> ApiResult<Json<Value>> {
    let db = app.db();
    require_admin(&app, &db, &a)?;
    let target = resolve(
        &db,
        q.get("account_id").map(String::as_str),
        q.get("email").map(String::as_str),
    )?
    .ok_or_else(|| ApiError::not_found("no such account"))?;
    Ok(Json(
        json!({"account_id": target, "grants": grants_of(&db, &target)?}),
    ))
}
