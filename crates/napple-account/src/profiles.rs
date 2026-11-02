//! Player profiles (up to 8 per account): the in-game identity. Fields, validation and CRUD; photos are in
//! `photos.rs`, nickname rules in `nicknames.rs`. Visibility itself is enforced by napple-social (and the photo route)
//! through `napple-visibility`; this module stores the `tier` and exposes `/internal/*` lookups for napple-social.

use crate::app::{AppState, Authed};
use crate::error::{ApiError, ApiResult};
use crate::nicknames;
use crate::saves::{delete_versions, flush_deletions};
use crate::util::{is_uuid, new_id, random_bytes};
use axum::Json;
use axum::extract::{Path, Query, State};
use axum::http::{HeaderMap, StatusCode};
use napple_visibility::Tier;
use rusqlite::functions::FunctionFlags;
use rusqlite::{Connection, OptionalExtension, params};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::collections::HashMap;

pub const MAX_BIO_CHARS: usize = 160;
pub const MAX_LINKS: usize = 3;
pub const MAX_ICON: i64 = 47;

/// Adds the profile columns, the friend-code generator and the unique indexes (idempotent; runs at every open).
pub fn migrate(c: &Connection) -> rusqlite::Result<()> {
    c.create_scalar_function("napple_friend_code", 0, FunctionFlags::SQLITE_UTF8, |_| {
        Ok(new_friend_code())
    })?;
    let mut have = std::collections::HashSet::new();
    {
        let mut st = c.prepare("PRAGMA table_info(profiles)")?;
        for n in st.query_map([], |r| r.get::<_, String>(1))? {
            have.insert(n?);
        }
    }
    for (name, ddl) in [
        ("bio", "TEXT NOT NULL DEFAULT ''"),
        ("links", "TEXT NOT NULL DEFAULT '[]'"),
        ("photo_key", "TEXT"),
        ("photo_updated_at", "INTEGER"),
        ("tier", "INTEGER NOT NULL DEFAULT 1"),
        ("friend_code", "TEXT"),
    ] {
        if !have.contains(name) {
            c.execute(&format!("ALTER TABLE profiles ADD COLUMN {name} {ddl}"), [])?;
        }
    }
    // Rows inserted by any code path (signup creates the first profile) get a friend code.
    c.execute_batch(
        "CREATE TRIGGER IF NOT EXISTS profiles_friend_code AFTER INSERT ON profiles WHEN NEW.friend_code IS NULL
         BEGIN UPDATE profiles SET friend_code = napple_friend_code() WHERE id = NEW.id; END;
         UPDATE profiles SET friend_code = napple_friend_code() WHERE friend_code IS NULL;
         CREATE UNIQUE INDEX IF NOT EXISTS profiles_friend_code_u ON profiles(friend_code);",
    )?;
    // Nickname uniqueness is enforced by `insert`/`patch_profile` (check + write under the one connection lock,
    // no await between), not by an index: the legacy email sign-in still inserts its own first profile.
    Ok(())
}

fn luhn_check_digit(digits: &[u8]) -> u8 {
    // Digits left of the check digit, doubling every second one starting from the rightmost.
    let sum: u32 = digits
        .iter()
        .rev()
        .enumerate()
        .map(|(i, &d)| {
            let d = d as u32;
            if i % 2 == 0 {
                let x = d * 2;
                if x > 9 { x - 9 } else { x }
            } else {
                d
            }
        })
        .sum();
    ((10 - sum % 10) % 10) as u8
}

/// `NP-` + 12 digits: 11 random digits and a Luhn check digit.
pub fn new_friend_code() -> String {
    let r = random_bytes::<11>();
    let digits: Vec<u8> = r.iter().map(|b| b % 10).collect();
    let check = luhn_check_digit(&digits);
    let mut s = String::from("NP-");
    for d in digits {
        s.push((b'0' + d) as char);
    }
    s.push((b'0' + check) as char);
    s
}

pub fn valid_friend_code(code: &str) -> bool {
    let Some(d) = code.strip_prefix("NP-") else {
        return false;
    };
    if d.len() != 12 || !d.bytes().all(|b| b.is_ascii_digit()) {
        return false;
    }
    let digits: Vec<u8> = d.bytes().map(|b| b - b'0').collect();
    luhn_check_digit(&digits[..11]) == digits[11]
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
pub struct Link {
    pub kind: String,
    pub value: String,
}

fn bad(field: &'static str, msg: &str) -> ApiError {
    ApiError::bad_request(format!("{field}: {msg}")).with("field", field)
}

fn hidden_char(c: char) -> bool {
    matches!(c, '\u{00AD}' | '\u{200B}'..='\u{200F}' | '\u{202A}'..='\u{202E}' | '\u{2060}'..='\u{2064}'
        | '\u{2066}'..='\u{206F}' | '\u{FEFF}' | '\u{FFF9}'..='\u{FFFB}' | '\u{E0000}'..='\u{E007F}')
}

pub fn validate_bio(b: &str) -> ApiResult<String> {
    if b.chars().count() > MAX_BIO_CHARS {
        return Err(bad("bio", "at most 160 characters"));
    }
    if b.chars()
        .any(|c| (c.is_control() && c != '\n') || hidden_char(c))
    {
        return Err(bad(
            "bio",
            "control or invisible characters are not allowed",
        ));
    }
    Ok(b.trim().to_string())
}

fn valid_https_url(v: &str) -> bool {
    let Some(rest) = v.strip_prefix("https://") else {
        return false;
    };
    if v.len() > 200 || !v.bytes().all(|b| (0x21..0x7f).contains(&b)) {
        return false;
    }
    let authority = rest.split(['/', '?', '#']).next().unwrap_or("");
    if authority.is_empty() || authority.contains('@') || authority.starts_with(':') {
        return false;
    }
    let (host, port) = match authority.rsplit_once(':') {
        Some((h, p)) => (h, Some(p)),
        None => (authority, None),
    };
    if let Some(p) = port
        && (p.is_empty() || p.len() > 5 || !p.bytes().all(|b| b.is_ascii_digit()))
    {
        return false;
    }
    !host.is_empty()
        && host.contains('.')
        && !host.starts_with(['.', '-'])
        && !host.ends_with(['.', '-'])
        && !host.contains("..")
        && host
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'-')
}

pub fn validate_links(links: &[Link]) -> ApiResult<Vec<Link>> {
    if links.len() > MAX_LINKS {
        return Err(bad("links", "at most 3 links"));
    }
    let mut out = Vec::new();
    for l in links {
        let v = l.value.trim();
        match l.kind.as_str() {
            "web" => {
                if !valid_https_url(v) {
                    return Err(bad("links", "web links must be https:// URLs"));
                }
            }
            "discord" | "instagram" | "x" => {
                let h = v.strip_prefix('@').unwrap_or(v);
                let ok = (2..=32).contains(&h.len())
                    && h.bytes()
                        .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'.');
                if !ok {
                    return Err(bad("links", "handles are 2-32 letters, digits, '_' or '.'"));
                }
                out.push(Link {
                    kind: l.kind.clone(),
                    value: h.to_string(),
                });
                continue;
            }
            _ => return Err(bad("links", "kind must be discord, instagram, x or web")),
        }
        out.push(Link {
            kind: l.kind.clone(),
            value: v.to_string(),
        });
    }
    Ok(out)
}

pub fn validate_icon(i: i64) -> ApiResult<i64> {
    if (0..=MAX_ICON).contains(&i) {
        Ok(i)
    } else {
        Err(bad("icon_id", "built-in icons are 0-47"))
    }
}

pub fn validate_tier(t: i64) -> ApiResult<i64> {
    Tier::from_i64(t)
        .map(Tier::as_i64)
        .ok_or_else(|| bad("tier", "1-5"))
}

pub const COLS: &str = "id, account_id, nickname, bio, links, icon_id, photo_key IS NOT NULL, photo_updated_at, tier, friend_code, created_at, updated_at";

pub struct Row {
    pub id: String,
    pub account_id: String,
    pub nickname: String,
    pub bio: String,
    pub links: Vec<Link>,
    pub icon_id: i64,
    pub has_photo: bool,
    pub photo_updated_at: Option<i64>,
    pub tier: i64,
    pub friend_code: String,
    pub created_at: i64,
    pub updated_at: i64,
}

pub fn row(r: &rusqlite::Row) -> rusqlite::Result<Row> {
    let links: String = r.get(4)?;
    Ok(Row {
        id: r.get(0)?,
        account_id: r.get(1)?,
        nickname: r.get(2)?,
        bio: r.get(3)?,
        links: serde_json::from_str(&links).unwrap_or_default(),
        icon_id: r.get(5)?,
        has_photo: r.get(6)?,
        photo_updated_at: r.get(7)?,
        tier: r.get(8)?,
        friend_code: r.get::<_, Option<String>>(9)?.unwrap_or_default(),
        created_at: r.get(10)?,
        updated_at: r.get(11)?,
    })
}

impl Row {
    fn photo_json(&self) -> Value {
        if self.has_photo {
            json!({"url": format!("/account/v1/profiles/{}/photo", self.id), "updated_at": self.photo_updated_at})
        } else {
            Value::Null
        }
    }
    /// What the owner sees.
    pub fn json(&self) -> Value {
        json!({"profile_id": self.id, "nickname": self.nickname, "bio": self.bio, "links": self.links,
               "icon_id": self.icon_id, "photo": self.photo_json(), "tier": self.tier,
               "friend_code": self.friend_code, "created_at": self.created_at, "updated_at": self.updated_at})
    }
    /// What napple-social gets (it applies the tier rules to this).
    pub fn internal_json(&self) -> Value {
        let mut v = self.json();
        v["account_id"] = json!(self.account_id);
        v
    }
}

pub fn get_row(db: &Connection, id: &str) -> rusqlite::Result<Option<Row>> {
    db.query_row(
        &format!("SELECT {COLS} FROM profiles WHERE id = ?1"),
        [id],
        row,
    )
    .optional()
}

/// Creates a profile row with defaults (used for the first profile at signup verification, and by `POST /profiles`).
/// The nickname must already be validated; a taken nickname is a 409.
pub fn insert(
    db: &Connection,
    account_id: &str,
    nickname: &str,
    icon_id: i64,
    now: i64,
) -> ApiResult<String> {
    if nicknames::taken(db, nickname, None)? {
        return Err(nicknames::taken_error());
    }
    let id = new_id();
    db.execute(
        "INSERT INTO profiles (id, account_id, nickname, icon_id, created_at, updated_at) VALUES (?1, ?2, ?3, ?4, ?5, ?5)",
        params![id, account_id, nickname, icon_id, now],
    )
    .map_err(nicknames::map_insert_error)?;
    Ok(id)
}

pub async fn list_profiles(State(app): State<AppState>, a: Authed) -> ApiResult<Json<Value>> {
    let db = app.db();
    let mut st = db.prepare(&format!(
        "SELECT {COLS} FROM profiles WHERE account_id = ?1 ORDER BY created_at, id"
    ))?;
    let v: Vec<Value> = st
        .query_map([&a.account_id], row)?
        .map(|r| r.map(|r| r.json()))
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
    #[serde(default)]
    bio: Option<String>,
    #[serde(default)]
    links: Option<Vec<Link>>,
    #[serde(default)]
    tier: Option<i64>,
}

pub async fn create_profile(
    State(app): State<AppState>,
    a: Authed,
    Json(b): Json<NewProfile>,
) -> ApiResult<(StatusCode, Json<Value>)> {
    let icon = validate_icon(b.icon_id)?;
    let bio = b.bio.as_deref().map(validate_bio).transpose()?;
    let links = b.links.as_deref().map(validate_links).transpose()?;
    let tier = b.tier.map(validate_tier).transpose()?;
    let now = app.now();
    let db = app.db();
    let nick = nicknames::validate(&db, &a.account_id, &b.nickname)?;
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
    let id = insert(&db, &a.account_id, &nick, icon, now)?;
    db.execute(
        "UPDATE profiles SET bio = ?1, links = ?2, tier = ?3 WHERE id = ?4",
        params![
            bio.unwrap_or_default(),
            serde_json::to_string(&links.unwrap_or_default()).unwrap_or_else(|_| "[]".into()),
            tier.unwrap_or(1),
            id
        ],
    )?;
    let r = get_row(&db, &id)?.ok_or_else(|| ApiError::internal("profile vanished"))?;
    Ok((StatusCode::CREATED, Json(r.json())))
}

pub async fn get_profile(
    State(app): State<AppState>,
    a: Authed,
    Path(id): Path<String>,
) -> ApiResult<Json<Value>> {
    let db = app.db();
    match get_row(&db, &id)? {
        Some(r) if r.account_id == a.account_id => Ok(Json(r.json())),
        _ => Err(ApiError::not_found("no such profile")),
    }
}

#[derive(Deserialize)]
pub struct PatchProfile {
    #[serde(default)]
    nickname: Option<String>,
    #[serde(default)]
    bio: Option<String>,
    #[serde(default)]
    links: Option<Vec<Link>>,
    #[serde(default)]
    icon_id: Option<i64>,
    #[serde(default)]
    tier: Option<i64>,
}

pub async fn patch_profile(
    State(app): State<AppState>,
    a: Authed,
    Path(id): Path<String>,
    Json(b): Json<PatchProfile>,
) -> ApiResult<Json<Value>> {
    let now = app.now();
    let bio = b.bio.as_deref().map(validate_bio).transpose()?;
    let links = b.links.as_deref().map(validate_links).transpose()?;
    let icon = b.icon_id.map(validate_icon).transpose()?;
    let tier = b.tier.map(validate_tier).transpose()?;
    let db = app.db();
    if !matches!(get_row(&db, &id)?, Some(r) if r.account_id == a.account_id) {
        return Err(ApiError::not_found("no such profile"));
    }
    let nick = match &b.nickname {
        // Re-saving the current name (any case change included) must not collide with itself.
        Some(n) => {
            let n = nicknames::validate(&db, &a.account_id, n)?;
            if nicknames::taken(&db, &n, Some(&id))? {
                return Err(nicknames::taken_error());
            }
            Some(n)
        }
        None => None,
    };
    let tx = db.unchecked_transaction()?;
    let w = nicknames::map_insert_error;
    if let Some(n) = &nick {
        tx.execute(
            "UPDATE profiles SET nickname = ?1 WHERE id = ?2",
            params![n, id],
        )
        .map_err(w)?;
    }
    if let Some(v) = &bio {
        tx.execute("UPDATE profiles SET bio = ?1 WHERE id = ?2", params![v, id])?;
    }
    if let Some(v) = &links {
        let s = serde_json::to_string(v).unwrap_or_else(|_| "[]".into());
        tx.execute(
            "UPDATE profiles SET links = ?1 WHERE id = ?2",
            params![s, id],
        )?;
    }
    if let Some(v) = icon {
        tx.execute(
            "UPDATE profiles SET icon_id = ?1 WHERE id = ?2",
            params![v, id],
        )?;
    }
    if let Some(v) = tier {
        tx.execute(
            "UPDATE profiles SET tier = ?1 WHERE id = ?2",
            params![v, id],
        )?;
    }
    tx.execute(
        "UPDATE profiles SET updated_at = ?1 WHERE id = ?2",
        params![now, id],
    )?;
    tx.commit()?;
    let r = get_row(&db, &id)?.ok_or_else(|| ApiError::internal("profile vanished"))?;
    Ok(Json(r.json()))
}

/// Refuses while the profile has cloud saves unless `?delete_saves=true`; removes the photo too.
pub async fn delete_profile(
    State(app): State<AppState>,
    a: Authed,
    Path(id): Path<String>,
    Query(q): Query<HashMap<String, String>>,
) -> ApiResult<StatusCode> {
    let now = app.now();
    let (keys, photo) = {
        let db = app.db();
        let cur = if is_uuid(&id) {
            get_row(&db, &id)?
        } else {
            None
        };
        if !matches!(&cur, Some(r) if r.account_id == a.account_id) {
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
        let photo: Option<String> =
            db.query_row("SELECT photo_key FROM profiles WHERE id = ?1", [&id], |r| {
                r.get(0)
            })?;
        let keys = delete_versions(
            &db,
            "account_id = ?1 AND profile_id = ?2",
            &[&a.account_id, &id],
            now,
        )?;
        db.execute("DELETE FROM profiles WHERE id = ?1", [&id])?;
        (keys, photo)
    };
    if let Some(k) = photo
        && let Err(e) = app.photos.delete(&k).await
    {
        crate::log::warn(&format!("photo delete {k}: {e}"));
    }
    flush_deletions(&app, keys).await;
    Ok(StatusCode::NO_CONTENT)
}

/// Photo object keys of every profile of an account (call before deleting the account, then `delete` each).
pub fn photo_keys_of_account(db: &Connection, account_id: &str) -> rusqlite::Result<Vec<String>> {
    let mut st = db.prepare(
        "SELECT photo_key FROM profiles WHERE account_id = ?1 AND photo_key IS NOT NULL",
    )?;
    st.query_map([account_id], |r| r.get(0))?.collect()
}

// ---- napple-social / napple-online lookups (shared secret, not user tokens) ----

/// `NAPPLE_INTERNAL_TOKEN` (>= 16 chars) in `x-napple-internal`; unset = the internal routes do not exist (404).
pub fn internal_auth(headers: &HeaderMap) -> ApiResult<()> {
    use subtle::ConstantTimeEq;
    let want = std::env::var("NAPPLE_INTERNAL_TOKEN").unwrap_or_default();
    let got = headers
        .get("x-napple-internal")
        .and_then(|v| v.to_str().ok())
        .unwrap_or("");
    if want.len() >= 16 && bool::from(want.as_bytes().ct_eq(got.as_bytes())) {
        Ok(())
    } else {
        Err(ApiError::not_found("not found"))
    }
}

pub async fn internal_get(
    State(app): State<AppState>,
    headers: HeaderMap,
    Path(id): Path<String>,
) -> ApiResult<Json<Value>> {
    internal_auth(&headers)?;
    let db = app.db();
    get_row(&db, &id)?
        .map(|r| Json(r.internal_json()))
        .ok_or_else(|| ApiError::not_found("no such profile"))
}

pub async fn internal_by_code(
    State(app): State<AppState>,
    headers: HeaderMap,
    Path(code): Path<String>,
) -> ApiResult<Json<Value>> {
    internal_auth(&headers)?;
    let code = code.to_ascii_uppercase();
    if !valid_friend_code(&code) {
        return Err(ApiError::not_found("no such profile"));
    }
    let db = app.db();
    db.query_row(
        &format!("SELECT {COLS} FROM profiles WHERE friend_code = ?1"),
        [&code],
        row,
    )
    .optional()?
    .map(|r| Json(r.internal_json()))
    .ok_or_else(|| ApiError::not_found("no such profile"))
}

/// `?prefix=ab&limit=20`: nickname prefix search (case-insensitive). The caller filters by tier.
pub async fn internal_search(
    State(app): State<AppState>,
    headers: HeaderMap,
    Query(q): Query<HashMap<String, String>>,
) -> ApiResult<Json<Value>> {
    internal_auth(&headers)?;
    let prefix = q.get("prefix").map(String::as_str).unwrap_or("");
    if prefix.is_empty() || prefix.len() > nicknames::MAX_LEN || !nicknames::valid_prefix(prefix) {
        return Ok(Json(json!({"profiles": []})));
    }
    let limit = q
        .get("limit")
        .and_then(|v| v.parse::<i64>().ok())
        .unwrap_or(20)
        .clamp(1, 50);
    let db = app.db();
    let mut st = db.prepare(&format!(
        "SELECT {COLS} FROM profiles WHERE nickname LIKE ?1 ESCAPE '\\' ORDER BY nickname COLLATE NOCASE LIMIT ?2"
    ))?;
    let like = format!("{}%", prefix.replace('_', "\\_"));
    let v: Vec<Value> = st
        .query_map(params![like, limit], row)?
        .map(|r| r.map(|r| r.internal_json()))
        .collect::<Result<_, _>>()?;
    Ok(Json(json!({"profiles": v})))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn friend_codes_are_luhn_valid() {
        for _ in 0..200 {
            let c = new_friend_code();
            assert!(valid_friend_code(&c), "{c}");
            assert_eq!(c.len(), 15);
        }
        assert!(!valid_friend_code("NP-000000000001"));
        assert!(valid_friend_code("NP-000000000000"));
        assert!(!valid_friend_code("XX-000000000000"));
        assert!(!valid_friend_code("NP-00000000000"));
        assert!(!valid_friend_code("NP-00000000000a"));
    }

    #[test]
    fn links_rules() {
        let l = |k: &str, v: &str| Link {
            kind: k.into(),
            value: v.into(),
        };
        assert!(validate_links(&[l("web", "https://napple.lat/me")]).is_ok());
        assert!(validate_links(&[l("web", "https://sub.example.com:8443/a?b=c")]).is_ok());
        for bad in [
            "http://example.com",
            "javascript:alert(1)",
            "https://",
            "https://user:pw@example.com",
            "https://example.com/a b",
            "https://localhost",
            "https://exa mple.com",
            "HTTPS://example.com",
            "ftp://example.com",
            "https://example.com:port/",
            "https://-bad.com",
            "https://\u{e9}xample.com",
            "//example.com",
        ] {
            assert!(validate_links(&[l("web", bad)]).is_err(), "{bad}");
        }
        assert_eq!(
            validate_links(&[l("x", "@neto_x")]).unwrap()[0].value,
            "neto_x"
        );
        assert!(validate_links(&[l("discord", "a")]).is_err());
        assert!(validate_links(&[l("tiktok", "abc")]).is_err());
        let four = vec![l("x", "abc"), l("x", "abd"), l("x", "abe"), l("x", "abf")];
        assert!(validate_links(&four).is_err());
    }

    #[test]
    fn bio_and_icon_and_tier() {
        assert!(validate_bio(&"a".repeat(160)).is_ok());
        assert!(validate_bio(&"a".repeat(161)).is_err());
        assert!(validate_bio("zero\u{200B}width").is_err());
        assert!(validate_bio("bell\u{7}").is_err());
        assert!(validate_bio("two\nlines").is_ok());
        assert!(
            validate_icon(47).is_ok() && validate_icon(48).is_err() && validate_icon(-1).is_err()
        );
        assert!(validate_tier(1).is_ok() && validate_tier(5).is_ok());
        assert!(validate_tier(0).is_err() && validate_tier(6).is_err());
    }
}
