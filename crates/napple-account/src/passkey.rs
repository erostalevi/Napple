//! Passkey (WebAuthn) registration for a signed-in account, discoverable-credential login, and passkey management.

use crate::app::{AppState, Authed, ClientIp, limit};
use crate::error::{ApiError, ApiResult};
use crate::signin::{DeviceIn, check_device, issue_tokens, upsert_device};
use crate::util::{b64_decode, b64url, clean_name, ct_eq, new_id, random_bytes};
use crate::webauthn::{ALG_EDDSA, ALG_ES256, verify_assertion, verify_registration};
use axum::Json;
use axum::extract::{Path, State};
use axum::http::StatusCode;
use rusqlite::{Connection, OptionalExtension, params};
use serde::Deserialize;
use serde_json::{Value, json};

const CEREMONY_TTL: i64 = 300;
const MAX_PASSKEYS: i64 = 16;

fn account_handle(account_id: &str) -> ApiResult<Vec<u8>> {
    Ok(uuid::Uuid::parse_str(account_id)
        .map_err(ApiError::internal)?
        .as_bytes()
        .to_vec())
}

fn new_challenge(
    db: &Connection,
    kind: &str,
    account_id: Option<&str>,
    now: i64,
) -> ApiResult<(String, [u8; 32])> {
    db.execute(
        "DELETE FROM webauthn_challenges WHERE expires_at < ?1",
        [now],
    )?;
    let id = new_id();
    let ch = random_bytes::<32>();
    db.execute(
        "INSERT INTO webauthn_challenges (id, challenge, kind, account_id, expires_at) VALUES (?1, ?2, ?3, ?4, ?5)",
        params![id, b64url(&ch), kind, account_id, now + CEREMONY_TTL],
    )?;
    Ok((id, ch))
}

/// Takes (single use) a ceremony challenge of `kind`; returns (challenge bytes, account id).
fn take_challenge(
    db: &Connection,
    id: &str,
    kind: &str,
    now: i64,
) -> ApiResult<(Vec<u8>, Option<String>)> {
    let row: Option<(String, String, Option<String>, i64)> = db
        .query_row(
            "SELECT challenge, kind, account_id, expires_at FROM webauthn_challenges WHERE id = ?1",
            [id],
            |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?)),
        )
        .optional()?;
    db.execute("DELETE FROM webauthn_challenges WHERE id = ?1", [id])?;
    match row {
        Some((ch, k, acc, exp)) if k == kind && now <= exp => {
            Ok((b64_decode(&ch).unwrap_or_default(), acc))
        }
        _ => Err(ApiError::bad_request("unknown or expired ceremony")
            .with("error_code", "ceremony_expired")),
    }
}

fn field(v: &Value, path: &[&str]) -> ApiResult<Vec<u8>> {
    let mut cur = v;
    for p in path {
        cur = &cur[*p];
    }
    cur.as_str().and_then(b64_decode).ok_or_else(|| {
        ApiError::bad_request(format!(
            "credential.{} missing or not base64url",
            path.join(".")
        ))
    })
}

pub async fn register_begin(State(app): State<AppState>, a: Authed) -> ApiResult<Json<Value>> {
    let now = app.now();
    let db = app.db();
    let (email, display): (String, String) = db.query_row(
        "SELECT email, display_name FROM accounts WHERE id = ?1",
        [&a.account_id],
        |r| Ok((r.get(0)?, r.get(1)?)),
    )?;
    let mut st = db.prepare("SELECT cred_id FROM passkeys WHERE account_id = ?1")?;
    let exclude: Vec<Value> = st
        .query_map([&a.account_id], |r| r.get::<_, String>(0))?
        .filter_map(Result::ok)
        .map(|id| json!({"type": "public-key", "id": id}))
        .collect();
    let (id, ch) = new_challenge(&db, "register", Some(&a.account_id), now)?;
    Ok(Json(json!({
        "challenge_id": id,
        "publicKey": {
            "rp": {"id": app.cfg.rp_id, "name": app.cfg.rp_name},
            "user": {"id": b64url(&account_handle(&a.account_id)?), "name": email, "displayName": display},
            "challenge": b64url(&ch),
            "pubKeyCredParams": [{"type": "public-key", "alg": ALG_EDDSA}, {"type": "public-key", "alg": ALG_ES256}],
            "timeout": CEREMONY_TTL * 1000,
            "attestation": "none",
            "authenticatorSelection": {"residentKey": "required", "requireResidentKey": true, "userVerification": "required"},
            "excludeCredentials": exclude,
        }
    })))
}

#[derive(Deserialize)]
pub struct RegisterFinish {
    challenge_id: String,
    #[serde(default)]
    name: Option<String>,
    credential: Value,
}

pub async fn register_finish(
    State(app): State<AppState>,
    a: Authed,
    Json(b): Json<RegisterFinish>,
) -> ApiResult<(StatusCode, Json<Value>)> {
    let now = app.now();
    let db = app.db();
    let (ch, acc) = take_challenge(&db, &b.challenge_id, "register", now)?;
    if acc.as_deref() != Some(a.account_id.as_str()) {
        return Err(ApiError::bad_request("ceremony belongs to another account"));
    }
    let raw_id = field(&b.credential, &["rawId"]).or_else(|_| field(&b.credential, &["id"]))?;
    let cd = field(&b.credential, &["response", "clientDataJSON"])?;
    let att = field(&b.credential, &["response", "attestationObject"])?;
    let cred = verify_registration(
        &cd,
        &att,
        &raw_id,
        &ch,
        &app.cfg.rp_id,
        &app.cfg.webauthn_origins,
    )
    .map_err(|e| {
        ApiError::bad_request(format!("passkey registration rejected: {e}"))
            .with("error_code", "passkey_rejected")
    })?;
    let count: i64 = db.query_row(
        "SELECT COUNT(*) FROM passkeys WHERE account_id = ?1",
        [&a.account_id],
        |r| r.get(0),
    )?;
    if count >= MAX_PASSKEYS {
        return Err(ApiError::conflict(
            "passkey_limit",
            "16 passkeys already; remove one first",
        ));
    }
    let name = b
        .name
        .as_deref()
        .and_then(|n| clean_name(n, 64))
        .unwrap_or_else(|| "Passkey".into());
    let cred_id = b64url(&cred.cred_id);
    let inserted = db.execute(
        "INSERT OR IGNORE INTO passkeys (cred_id, account_id, cose_key, alg, sign_count, name, aaguid, backup_eligible, created_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
        params![cred_id, a.account_id, cred.cose_key, cred.alg, cred.sign_count, name, hex::encode(cred.aaguid), cred.backup_eligible, now],
    )?;
    if inserted == 0 {
        return Err(ApiError::conflict(
            "passkey_exists",
            "this passkey is already registered",
        ));
    }
    Ok((
        StatusCode::CREATED,
        Json(json!({"credential_id": cred_id, "name": name, "created_at": now})),
    ))
}

pub async fn login_begin(State(app): State<AppState>, ip: ClientIp) -> ApiResult<Json<Value>> {
    limit(&app, "auth-ip", &ip.0, app.cfg.auth_ip_per_min, 60)?;
    let now = app.now();
    let (id, ch) = new_challenge(&app.db(), "login", None, now)?;
    // Discoverable credentials: no allowCredentials, so nothing about which accounts exist leaks.
    Ok(Json(json!({
        "challenge_id": id,
        "publicKey": {"challenge": b64url(&ch), "rpId": app.cfg.rp_id, "timeout": CEREMONY_TTL * 1000, "userVerification": "required", "allowCredentials": []}
    })))
}

#[derive(Deserialize)]
pub struct LoginFinish {
    challenge_id: String,
    credential: Value,
    device: DeviceIn,
}

pub async fn login_finish(
    State(app): State<AppState>,
    ip: ClientIp,
    Json(b): Json<LoginFinish>,
) -> ApiResult<Json<Value>> {
    limit(&app, "auth-ip", &ip.0, app.cfg.auth_ip_per_min, 60)?;
    check_device(&b.device)?;
    let now = app.now();
    let db = app.db();
    let (ch, _) = take_challenge(&db, &b.challenge_id, "login", now)?;
    let fail = || {
        ApiError::new(
            StatusCode::UNAUTHORIZED,
            "passkey_failed",
            "passkey sign-in failed",
        )
    };
    let raw_id = field(&b.credential, &["rawId"]).or_else(|_| field(&b.credential, &["id"]))?;
    let cred_id = b64url(&raw_id);
    let row: Option<(String, Vec<u8>, u32)> = db
        .query_row(
            "SELECT account_id, cose_key, sign_count FROM passkeys WHERE cred_id = ?1",
            [&cred_id],
            |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
        )
        .optional()?;
    let Some((account_id, cose, count)) = row else {
        return Err(fail());
    };
    if let Some(uh) = b.credential["response"]["userHandle"]
        .as_str()
        .filter(|s| !s.is_empty())
        && !ct_eq(
            &b64_decode(uh).unwrap_or_default(),
            &account_handle(&account_id)?,
        )
    {
        return Err(fail());
    }
    let cd = field(&b.credential, &["response", "clientDataJSON"])?;
    let ad = field(&b.credential, &["response", "authenticatorData"])?;
    let sig = field(&b.credential, &["response", "signature"])?;
    let new_count = verify_assertion(
        &cd,
        &ad,
        &sig,
        &ch,
        &app.cfg.rp_id,
        &app.cfg.webauthn_origins,
        &cose,
        count,
    )
    .map_err(|e| {
        crate::log::warn(&format!("passkey assertion rejected: {e}"));
        fail()
    })?;
    db.execute(
        "UPDATE passkeys SET sign_count = ?1, last_used = ?2 WHERE cred_id = ?3",
        params![new_count, now, cred_id],
    )?;
    let device_id = upsert_device(&db, &account_id, &b.device, now)?;
    let mut v = issue_tokens(&app, &db, &account_id, &device_id, now)?;
    v["new_account"] = json!(false);
    Ok(Json(v))
}

pub async fn list(State(app): State<AppState>, a: Authed) -> ApiResult<Json<Value>> {
    let db = app.db();
    let mut st = db.prepare("SELECT cred_id, name, created_at, last_used, backup_eligible FROM passkeys WHERE account_id = ?1 ORDER BY created_at")?;
    let v: Vec<Value> = st
        .query_map([&a.account_id], |r| {
            Ok(json!({"credential_id": r.get::<_, String>(0)?, "name": r.get::<_, String>(1)?, "created_at": r.get::<_, i64>(2)?,
                      "last_used": r.get::<_, Option<i64>>(3)?, "synced": r.get::<_, bool>(4)?}))
        })?
        .collect::<Result<_, _>>()?;
    Ok(Json(json!({"passkeys": v})))
}

pub async fn remove(
    State(app): State<AppState>,
    a: Authed,
    Path(id): Path<String>,
) -> ApiResult<StatusCode> {
    let n = app.db().execute(
        "DELETE FROM passkeys WHERE cred_id = ?1 AND account_id = ?2",
        params![id, a.account_id],
    )?;
    if n == 0 {
        return Err(ApiError::not_found("no such passkey"));
    }
    Ok(StatusCode::NO_CONTENT)
}
