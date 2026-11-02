//! Tier-aware social features: search, the public profile view, stats, shared lobbies, recent players and reports
//! (ONLINE-CONTRACT.md "Social" and the visibility table). Every read goes through `visibility::can_view`; a hidden
//! profile looks exactly like a missing one (404), and a block hides both ways.
//!
//! Service endpoints (`/encounters`, `/internal/*`) are called by napple-online with `X-Napple-Service`; they are
//! off (401) unless `NAPPLE_SOCIAL_SERVICE_TOKEN` is set.

use std::collections::{BTreeMap, HashMap};

use axum::Json;
use axum::extract::{Query, State};
use axum::http::{HeaderMap, StatusCode};
use axum::response::{IntoResponse, Response};
use rand::Rng;
use rusqlite::{Connection, OptionalExtension, params};
use serde::Deserialize;
use serde_json::{Value, json};

use crate::api::{ApiJson, ApiPath};
use crate::db::{self, ProfileRow, ProfileView};
use crate::error::ApiError;
use crate::visibility::{Kind, Tier, Viewer, can_view};
use crate::{AppState, Caller, codes};

type R<T> = Result<T, ApiError>;

/// A match-mate stays "a shared lobby" for friend requests this long after the match.
pub const ENCOUNTER_LOBBY_MS: i64 = 24 * 3_600_000;
/// Recent players are kept this long.
pub const RECENT_RETENTION_MS: i64 = 30 * 86_400_000;
const RECENT_PER_PROFILE: i64 = 100;
const SEARCH_RESULTS: usize = 20;
const SEARCH_CANDIDATES: i64 = 100;
const DEFAULT_LOBBY_TTL_SECS: i64 = 2 * 3600;
const MAX_LOBBY_TTL_SECS: i64 = 86_400;
const MAX_LOBBY_MEMBERS: usize = 64;
const MAX_STAT_KEYS_PER_CALL: usize = 16;
const MAX_STAT_KEYS_PER_TITLE: i64 = 32;
const MAX_STAT_DELTA: i64 = 1_000_000_000;
const MAX_STAT_VALUE: i64 = 1_000_000_000_000;
const MAX_REASON: usize = 500;
const MAX_ID_TEXT: usize = 128;

// ---------------------------------------------------------------- service credential

fn ct_eq(a: &[u8], b: &[u8]) -> bool {
    let mut diff = (a.len() ^ b.len()) as u8;
    for i in 0..a.len().max(b.len()) {
        diff |= a.get(i).copied().unwrap_or(0) ^ b.get(i).copied().unwrap_or(0);
    }
    diff == 0
}

pub fn service_auth(s: &AppState, headers: &HeaderMap) -> R<()> {
    let denied = || {
        ApiError::unauthorized(
            "invalid_service_token",
            "X-Napple-Service credential required",
        )
    };
    let want = s.config.service_token.as_deref().ok_or_else(denied)?;
    let got = headers
        .get("x-napple-service")
        .and_then(|v| v.to_str().ok())
        .ok_or_else(denied)?;
    if ct_eq(want.as_bytes(), got.as_bytes()) {
        Ok(())
    } else {
        Err(denied())
    }
}

// ---------------------------------------------------------------- profile snapshots

impl AppState {
    /// Re-reads stale profiles (tier, nickname, photo) from napple-account with the service credential. A failed
    /// lookup keeps the last snapshot: a short account outage must not flip anybody's visibility.
    pub async fn refresh_profiles(&self, ids: &[String]) {
        if self.config.account_service_token.is_none() || ids.is_empty() {
            return;
        }
        let now = self.now_ms();
        let ttl = self.config.profile_ttl_secs * 1000;
        let ids: Vec<String> = ids.to_vec();
        let stale: Vec<String> = match self
            .db
            .call(move |c| {
                let mut out = Vec::new();
                for id in ids {
                    if let Some(p) = db::profile(c, &id)?
                        && now - p.synced_ms >= ttl
                    {
                        out.push(id);
                    }
                }
                Ok(out)
            })
            .await
        {
            Ok(v) => v,
            Err(_) => return,
        };
        if stale.is_empty() {
            return;
        }
        let lookups = stale.iter().map(|id| self.auth.directory_lookup(id, now));
        let found = futures_util::future::join_all(lookups).await;
        let found: Vec<_> = found.into_iter().flatten().collect();
        let _ = self
            .db
            .tx(move |c| {
                for p in &found {
                    c.execute(
                        "UPDATE profiles SET nickname = ?2, icon_id = ?3, tier = ?4, photo = ?5, synced_ms = ?6 WHERE profile_id = ?1",
                        params![p.profile_id, p.nickname, p.icon_id, p.tier, p.photo, p.as_of_ms],
                    )?;
                }
                Ok(())
            })
            .await;
        // A lookup that failed leaves synced_ms alone, so the next call retries.
    }
}

// ---------------------------------------------------------------- relations

fn is_blocked_either(c: &Connection, a: &str, b: &str) -> R<bool> {
    Ok(db::is_blocked(c, a, b)? || db::is_blocked(c, b, a)?)
}

/// Both are in one live lobby (the given one, or any), or played together in the last 24 h.
fn shared_link(c: &Connection, a: &str, b: &str, lobby: Option<&str>, now: i64) -> R<bool> {
    let in_lobby: bool = match lobby {
        Some(l) => c.query_row(
            "SELECT (SELECT COUNT(*) FROM lobbies WHERE lobby_id = ?1 AND profile_id = ?2 AND expires_ms > ?4)
                  + (SELECT COUNT(*) FROM lobbies WHERE lobby_id = ?1 AND profile_id = ?3 AND expires_ms > ?4) = 2",
            params![l, a, b, now],
            |r| r.get(0),
        )?,
        None => c.query_row(
            "SELECT EXISTS(SELECT 1 FROM lobbies x JOIN lobbies y ON x.lobby_id = y.lobby_id
              WHERE x.profile_id = ?1 AND y.profile_id = ?2 AND x.expires_ms > ?3 AND y.expires_ms > ?3)",
            params![a, b, now],
            |r| r.get(0),
        )?,
    };
    if in_lobby {
        return Ok(true);
    }
    if lobby.is_some() {
        return Ok(false);
    }
    Ok(c.query_row(
        "SELECT EXISTS(SELECT 1 FROM encounters WHERE profile_id = ?1 AND other_profile = ?2 AND at_ms > ?3)",
        params![a, b, now - ENCOUNTER_LOBBY_MS],
        |r| r.get(0),
    )?)
}

pub fn viewer_of(
    c: &Connection,
    me: &str,
    target: &str,
    lobby: Option<&str>,
    now: i64,
) -> R<Viewer> {
    Ok(Viewer {
        is_self: me == target,
        is_friend: db::are_friends(c, me, target)?,
        blocked: is_blocked_either(c, me, target)?,
        shared_lobby: shared_link(c, me, target, lobby, now)?,
    })
}

fn friend_status(c: &Connection, me: &str, other: &str) -> R<&'static str> {
    Ok(if me == other {
        "self"
    } else if db::are_friends(c, me, other)? {
        "friend"
    } else if db::request_between(c, me, other)?.is_some() {
        "pending_out"
    } else if db::request_between(c, other, me)?.is_some() {
        "pending_in"
    } else {
        "none"
    })
}

fn can_request(c: &Connection, me: &str, p: &ProfileRow, v: Viewer) -> R<bool> {
    Ok(!v.is_self
        && can_view(v, p.tier, Kind::FriendRequest)
        && p.accept_requests
        && friend_status(c, me, &p.profile_id)? == "none")
}

// ---------------------------------------------------------------- search

#[derive(Deserialize)]
pub struct SearchQuery {
    q: Option<String>,
}

fn like_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 1);
    for ch in s.chars() {
        if matches!(ch, '%' | '_' | '\\') {
            out.push('\\');
        }
        out.push(ch);
    }
    out.push('%');
    out
}

pub async fn search(
    State(s): State<AppState>,
    caller: Caller,
    Query(q): Query<SearchQuery>,
) -> R<Json<Value>> {
    let now = s.now_ms();
    s.limits.search.check(&caller.profile_id, now)?;
    let q = q.q.unwrap_or_default().trim().to_string();
    let me = caller.profile_id.clone();
    let code = codes::parse(&q);
    let pattern = like_escape(&q.to_lowercase());
    let long_enough = q.chars().count() >= 2;
    let by_code = code.is_some();
    let candidates: Vec<String> = {
        let me = me.clone();
        s.db.call(move |c| {
            let mut out = Vec::new();
            if let Some(code) = code {
                if let Some(p) = db::profile_by_code(c, &code)? {
                    out.push(p.profile_id);
                }
            } else if long_enough {
                let mut st = c.prepare(
                    "SELECT profile_id FROM profiles WHERE LOWER(nickname) LIKE ?1 ESCAPE '\\' AND profile_id != ?2
                     ORDER BY LOWER(nickname), profile_id LIMIT ?3",
                )?;
                let rows = st.query_map(params![pattern, me, SEARCH_CANDIDATES], |r| r.get(0))?;
                for r in rows {
                    out.push(r?);
                }
            }
            Ok(out)
        })
        .await?
    };
    s.refresh_profiles(&candidates).await;
    let results =
        s.db.call(move |c| {
            let mut out = Vec::new();
            for id in candidates {
                let Some(p) = db::profile(c, &id)? else {
                    continue;
                };
                // The nickname may have changed since the candidate query.
                if !by_code && !p.nickname.to_lowercase().starts_with(&q.to_lowercase()) {
                    continue;
                }
                let v = viewer_of(c, &me, &id, None, now)?;
                if !can_view(v, p.tier, Kind::Search) {
                    continue;
                }
                out.push(json!({
                    "profile": ProfileView::from(&p),
                    "friend_status": friend_status(c, &me, &id)?,
                    "can_friend_request": can_request(c, &me, &p, v)?,
                }));
                if out.len() >= SEARCH_RESULTS {
                    break;
                }
            }
            Ok(out)
        })
        .await?;
    Ok(Json(json!({ "results": results })))
}

// ---------------------------------------------------------------- public view

fn parse_id(raw: &str, what: &'static str) -> R<String> {
    uuid::Uuid::parse_str(raw.trim())
        .map(|u| u.hyphenated().to_string())
        .map_err(|_| ApiError::bad_request("invalid_id", format!("{what} must be a UUID")))
}

fn not_found_profile() -> ApiError {
    ApiError::not_found("profile_not_found", "no such Napple profile")
}

pub async fn public_profile(
    State(s): State<AppState>,
    caller: Caller,
    ApiPath(id): ApiPath<String>,
) -> R<Json<Value>> {
    let id = parse_id(&id, "profile id")?;
    s.refresh_profiles(std::slice::from_ref(&id)).await;
    let (me, now, timeout) = (caller.profile_id.clone(), s.now_ms(), s.timeout_ms());
    let v =
        s.db.call(move |c| {
            let p = db::profile(c, &id)?.ok_or_else(not_found_profile)?;
            let v = viewer_of(c, &me, &id, None, now)?;
            if !can_view(v, p.tier, Kind::Card) {
                return Err(not_found_profile());
            }
            let mut out = json!({
                "profile": ProfileView::from(&p),
                "friend_status": friend_status(c, &me, &id)?,
                "can_friend_request": can_request(c, &me, &p, v)?,
            });
            if can_view(v, p.tier, Kind::Details) {
                let pr = db::presence(c, &id)?;
                let presence = if v.is_self {
                    db::own_presence(&p, pr.as_ref(), now, timeout)
                } else {
                    db::friend_presence(&p, pr.as_ref(), now, timeout)
                };
                out["presence"] = serde_json::to_value(presence).unwrap_or(Value::Null);
                out["stats_visible"] = json!(true);
            }
            if v.is_self {
                out["tier"] = json!(p.tier.as_i64());
            }
            Ok(out)
        })
        .await?;
    Ok(Json(v))
}

// ---------------------------------------------------------------- stats

fn norm_title(t: &str) -> R<String> {
    let t = t.trim();
    if t.len() == 16 && t.chars().all(|c| c.is_ascii_hexdigit()) {
        Ok(t.to_ascii_uppercase())
    } else {
        Err(ApiError::bad_request(
            "invalid_title",
            "title_id must be 16 hex digits",
        ))
    }
}

fn parse_deltas(v: &Value) -> R<Vec<(String, i64)>> {
    let bad = |m: &str| ApiError::bad_request("invalid_stats", m.to_string());
    let obj = v
        .as_object()
        .ok_or_else(|| bad("deltas must be an object"))?;
    if obj.is_empty() || obj.len() > MAX_STAT_KEYS_PER_CALL {
        return Err(bad("1 to 16 stat keys per call"));
    }
    let mut out = Vec::new();
    for (k, d) in obj {
        if k.is_empty()
            || k.len() > 48
            || !k
                .chars()
                .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_' || c == '.')
        {
            return Err(bad("stat keys are 1-48 chars of a-z 0-9 _ ."));
        }
        let d = d
            .as_i64()
            .filter(|d| d.abs() <= MAX_STAT_DELTA)
            .ok_or_else(|| bad("deltas are integers within +-1e9"))?;
        out.push((k.clone(), d));
    }
    Ok(out)
}

fn apply_deltas(c: &Connection, profile: &str, title: &str, deltas: &[(String, i64)]) -> R<()> {
    for (k, d) in deltas {
        let exists = c
            .query_row(
                "SELECT value FROM stats WHERE profile_id = ?1 AND title_id = ?2 AND key = ?3",
                params![profile, title, k],
                |r| r.get::<_, i64>(0),
            )
            .optional()?;
        if exists.is_none() {
            let n: i64 = c.query_row(
                "SELECT COUNT(*) FROM stats WHERE profile_id = ?1 AND title_id = ?2",
                params![profile, title],
                |r| r.get(0),
            )?;
            if n >= MAX_STAT_KEYS_PER_TITLE {
                return Err(ApiError::bad_request(
                    "invalid_stats",
                    format!("at most {MAX_STAT_KEYS_PER_TITLE} stat keys per title"),
                ));
            }
        }
        let v = (exists.unwrap_or(0) + d).clamp(-MAX_STAT_VALUE, MAX_STAT_VALUE);
        c.execute(
            "INSERT INTO stats (profile_id, title_id, key, value) VALUES (?1, ?2, ?3, ?4)
             ON CONFLICT(profile_id, title_id, key) DO UPDATE SET value = excluded.value",
            params![profile, title, k, v],
        )?;
    }
    Ok(())
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct StatsBody {
    title_id: String,
    deltas: Value,
}

pub async fn post_stats(
    State(s): State<AppState>,
    caller: Caller,
    ApiJson(body): ApiJson<StatsBody>,
) -> R<Json<Value>> {
    s.limits.stats.check(&caller.profile_id, s.now_ms())?;
    let title = norm_title(&body.title_id)?;
    let deltas = parse_deltas(&body.deltas)?;
    let me = caller.profile_id.clone();
    let v =
        s.db.tx(move |c| {
            apply_deltas(c, &me, &title, &deltas)?;
            read_stats(c, &me, Some(&title))
        })
        .await?;
    Ok(Json(v))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ServiceStatsBody {
    profile_id: String,
    title_id: String,
    deltas: Value,
}

pub async fn internal_stats(
    State(s): State<AppState>,
    headers: HeaderMap,
    ApiJson(body): ApiJson<ServiceStatsBody>,
) -> R<Json<Value>> {
    service_auth(&s, &headers)?;
    let id = parse_id(&body.profile_id, "profile_id")?;
    let title = norm_title(&body.title_id)?;
    let deltas = parse_deltas(&body.deltas)?;
    let v =
        s.db.tx(move |c| {
            db::profile(c, &id)?.ok_or_else(not_found_profile)?;
            apply_deltas(c, &id, &title, &deltas)?;
            read_stats(c, &id, Some(&title))
        })
        .await?;
    Ok(Json(v))
}

fn read_stats(c: &Connection, id: &str, title: Option<&str>) -> R<Value> {
    let mut st = c.prepare(
        "SELECT title_id, key, value FROM stats WHERE profile_id = ?1 AND (?2 IS NULL OR title_id = ?2) ORDER BY title_id, key",
    )?;
    let rows = st.query_map(params![id, title], |r| {
        Ok((
            r.get::<_, String>(0)?,
            r.get::<_, String>(1)?,
            r.get::<_, i64>(2)?,
        ))
    })?;
    let mut titles: BTreeMap<String, serde_json::Map<String, Value>> = BTreeMap::new();
    for r in rows {
        let (t, k, v) = r?;
        titles.entry(t).or_default().insert(k, json!(v));
    }
    Ok(json!({ "profile_id": id, "titles": titles }))
}

#[derive(Deserialize)]
pub struct StatsQuery {
    title_id: Option<String>,
}

pub async fn get_stats(
    State(s): State<AppState>,
    caller: Caller,
    ApiPath(id): ApiPath<String>,
    Query(q): Query<StatsQuery>,
) -> R<Json<Value>> {
    let id = parse_id(&id, "profile id")?;
    let title = q.title_id.as_deref().map(norm_title).transpose()?;
    s.refresh_profiles(std::slice::from_ref(&id)).await;
    let (me, now) = (caller.profile_id.clone(), s.now_ms());
    let v =
        s.db.call(move |c| {
            let p = db::profile(c, &id)?.ok_or_else(not_found_profile)?;
            let v = viewer_of(c, &me, &id, None, now)?;
            if !can_view(v, p.tier, Kind::Details) {
                return Err(not_found_profile());
            }
            read_stats(c, &id, title.as_deref())
        })
        .await?;
    Ok(Json(v))
}

// ---------------------------------------------------------------- shared lobbies (service-registered)

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct LobbyMember {
    profile_id: String,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct LobbyBody {
    lobby_id: String,
    members: Vec<LobbyMember>,
    ttl_secs: Option<i64>,
}

fn check_ident(v: &str, what: &'static str) -> R<String> {
    let v = v.trim();
    if v.is_empty() || v.len() > MAX_ID_TEXT || v.chars().any(char::is_control) {
        return Err(ApiError::bad_request(
            "invalid_id",
            format!("{what} must be 1-{MAX_ID_TEXT} printable characters"),
        ));
    }
    Ok(v.to_string())
}

fn new_alias(taken: &std::collections::HashSet<String>) -> String {
    const ABC: &[u8] = b"ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    let mut rng = rand::thread_rng();
    loop {
        let tail: String = (0..4)
            .map(|_| ABC[rng.gen_range(0..ABC.len())] as char)
            .collect();
        let a = format!("Bot-{tail}");
        if !taken.contains(&a) {
            return a;
        }
    }
}

/// napple-online registers who is in a lobby/room. Re-registering a lobby keeps the aliases of the members that stay.
pub async fn internal_lobby(
    State(s): State<AppState>,
    headers: HeaderMap,
    ApiJson(body): ApiJson<LobbyBody>,
) -> R<Json<Value>> {
    service_auth(&s, &headers)?;
    let lobby = check_ident(&body.lobby_id, "lobby_id")?;
    if body.members.is_empty() || body.members.len() > MAX_LOBBY_MEMBERS {
        return Err(ApiError::bad_request(
            "invalid_lobby",
            format!("1 to {MAX_LOBBY_MEMBERS} members"),
        ));
    }
    let mut ids = Vec::new();
    for m in &body.members {
        let id = parse_id(&m.profile_id, "profile_id")?;
        if !ids.contains(&id) {
            ids.push(id);
        }
    }
    let ttl = body
        .ttl_secs
        .unwrap_or(DEFAULT_LOBBY_TTL_SECS)
        .clamp(1, MAX_LOBBY_TTL_SECS);
    let now = s.now_ms();
    let expires = now + ttl * 1000;
    let out = s
        .db
        .tx(move |c| {
            let mut old: HashMap<String, String> = HashMap::new();
            {
                let mut st = c.prepare("SELECT profile_id, alias FROM lobbies WHERE lobby_id = ?1")?;
                for r in st.query_map([&lobby], |r| Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?)))? {
                    let (p, a) = r?;
                    old.insert(p, a);
                }
            }
            c.execute("DELETE FROM lobbies WHERE lobby_id = ?1", [&lobby])?;
            let mut taken: std::collections::HashSet<String> = ids
                .iter()
                .filter_map(|i| old.get(i).cloned())
                .collect();
            let mut members = Vec::new();
            for id in &ids {
                let alias = match old.get(id) {
                    Some(a) => a.clone(),
                    None => {
                        let a = new_alias(&taken);
                        taken.insert(a.clone());
                        a
                    }
                };
                c.execute(
                    "INSERT INTO lobbies (lobby_id, profile_id, alias, expires_ms) VALUES (?1, ?2, ?3, ?4)",
                    params![lobby, id, alias, expires],
                )?;
                members.push(json!({"profile_id": id, "alias": alias}));
            }
            Ok(json!({"lobby_id": lobby, "expires_ms": expires, "members": members}))
        })
        .await?;
    Ok(Json(out))
}

pub async fn lobby_view(
    State(s): State<AppState>,
    caller: Caller,
    ApiPath(lobby): ApiPath<String>,
) -> R<Json<Value>> {
    let lobby = check_ident(&lobby, "lobby_id")?;
    let (me, now) = (caller.profile_id.clone(), s.now_ms());
    let ids: Vec<String> = {
        let (lobby, me) = (lobby.clone(), me.clone());
        s.db.call(move |c| {
            let live: bool = c.query_row(
                "SELECT EXISTS(SELECT 1 FROM lobbies WHERE lobby_id = ?1 AND profile_id = ?2 AND expires_ms > ?3)",
                params![lobby, me, now],
                |r| r.get(0),
            )?;
            if !live {
                return Err(ApiError::not_found("lobby_not_found", "no such lobby"));
            }
            let mut st = c.prepare("SELECT profile_id FROM lobbies WHERE lobby_id = ?1 ORDER BY alias")?;
            let rows = st.query_map([&lobby], |r| r.get(0))?;
            Ok(rows.collect::<Result<Vec<String>, _>>()?)
        })
        .await?
    };
    s.refresh_profiles(&ids).await;
    let v = s
        .db
        .call(move |c| {
            let mut members = Vec::new();
            for id in ids {
                let Some(p) = db::profile(c, &id)? else { continue };
                let alias: String = c.query_row(
                    "SELECT alias FROM lobbies WHERE lobby_id = ?1 AND profile_id = ?2",
                    params![lobby, id],
                    |r| r.get(0),
                )?;
                let v = viewer_of(c, &me, &id, Some(&lobby), now)?;
                if v.is_self {
                    members.push(json!({
                        "self": true, "ghost": false, "alias": alias,
                        "profile_id": id, "nickname": p.nickname, "icon_id": p.icon_id, "photo": p.photo,
                    }));
                    continue;
                }
                if v.blocked {
                    continue;
                }
                if can_view(v, p.tier, Kind::LobbyReal) {
                    members.push(json!({
                        "self": false, "ghost": false, "alias": alias,
                        "profile_id": id, "nickname": p.nickname, "icon_id": p.icon_id, "photo": p.photo,
                        "friend_status": friend_status(c, &me, &id)?,
                        "can_friend_request": can_request(c, &me, &p, v)?,
                    }));
                } else {
                    members.push(json!({"self": false, "ghost": true, "alias": alias}));
                }
            }
            Ok(json!({"lobby_id": lobby, "members": members}))
        })
        .await?;
    Ok(Json(v))
}

// ---------------------------------------------------------------- encounters / recent players

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct EncounterBody {
    profile_ids: Vec<String>,
    title_id: String,
    match_id: String,
}

/// napple-online reports who played together after a match. Ghosts and blocked pairs leave no trace.
pub async fn post_encounters(
    State(s): State<AppState>,
    headers: HeaderMap,
    ApiJson(body): ApiJson<EncounterBody>,
) -> R<Json<Value>> {
    service_auth(&s, &headers)?;
    let title = norm_title(&body.title_id)?;
    let match_id = check_ident(&body.match_id, "match_id")?;
    if body.profile_ids.len() < 2 || body.profile_ids.len() > MAX_LOBBY_MEMBERS {
        return Err(ApiError::bad_request(
            "invalid_encounter",
            format!("2 to {MAX_LOBBY_MEMBERS} profile_ids"),
        ));
    }
    let mut ids = Vec::new();
    for raw in &body.profile_ids {
        let id = parse_id(raw, "profile id")?;
        if !ids.contains(&id) {
            ids.push(id);
        }
    }
    s.refresh_profiles(&ids).await;
    let now = s.now_ms();
    let recorded = s
        .db
        .tx(move |c| {
            let mut known = Vec::new();
            for id in &ids {
                if let Some(p) = db::profile(c, id)?
                    && p.tier != Tier::Ghost
                {
                    known.push(p.profile_id);
                }
            }
            let mut n = 0;
            for a in &known {
                for b in &known {
                    if a == b || is_blocked_either(c, a, b)? {
                        continue;
                    }
                    c.execute(
                        "INSERT INTO encounters (profile_id, other_profile, title_id, match_id, at_ms) VALUES (?1, ?2, ?3, ?4, ?5)
                         ON CONFLICT(profile_id, other_profile) DO UPDATE SET title_id = excluded.title_id, match_id = excluded.match_id, at_ms = excluded.at_ms",
                        params![a, b, title, match_id, now],
                    )?;
                    n += 1;
                }
                c.execute(
                    "DELETE FROM encounters WHERE profile_id = ?1 AND other_profile NOT IN
                     (SELECT other_profile FROM encounters WHERE profile_id = ?1 ORDER BY at_ms DESC LIMIT ?2)",
                    params![a, RECENT_PER_PROFILE],
                )?;
            }
            Ok(n)
        })
        .await?;
    Ok(Json(json!({ "recorded": recorded })))
}

pub async fn recent(State(s): State<AppState>, caller: Caller) -> R<Json<Value>> {
    let (me, now) = (caller.profile_id.clone(), s.now_ms());
    let rows: Vec<(String, String, String, i64)> = {
        let me = me.clone();
        s.db.call(move |c| {
            let mut st = c.prepare(
                "SELECT other_profile, title_id, match_id, at_ms FROM encounters WHERE profile_id = ?1 AND at_ms > ?2
                 ORDER BY at_ms DESC LIMIT ?3",
            )?;
            let rows = st.query_map(params![me, now - RECENT_RETENTION_MS, RECENT_PER_PROFILE], |r| {
                Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?))
            })?;
            Ok(rows.collect::<Result<Vec<_>, _>>()?)
        })
        .await?
    };
    let ids: Vec<String> = rows.iter().map(|r| r.0.clone()).collect();
    s.refresh_profiles(&ids).await;
    let out =
        s.db.call(move |c| {
            let mut out = Vec::new();
            for (id, title, match_id, at) in rows {
                let Some(p) = db::profile(c, &id)? else {
                    continue;
                };
                let mut v = viewer_of(c, &me, &id, None, now)?;
                if !can_view(v, p.tier, Kind::Recent) {
                    continue;
                }
                v.shared_lobby = now - at < ENCOUNTER_LOBBY_MS;
                out.push(json!({
                    "profile": ProfileView::from(&p),
                    "title_id": title,
                    "match_id": match_id,
                    "last_played_ms": at,
                    "friend_status": friend_status(c, &me, &id)?,
                    "can_friend_request": can_request(c, &me, &p, v)?,
                }));
            }
            Ok(out)
        })
        .await?;
    Ok(Json(json!({ "recent": out })))
}

// ---------------------------------------------------------------- reports

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ReportBody {
    profile_id: Option<String>,
    lobby_id: Option<String>,
    alias: Option<String>,
    reason: String,
    match_id: Option<String>,
}

pub async fn post_report(
    State(s): State<AppState>,
    caller: Caller,
    ApiJson(body): ApiJson<ReportBody>,
) -> R<Response> {
    let reason = body.reason.trim().to_string();
    if reason.is_empty()
        || reason.chars().count() > MAX_REASON
        || reason.chars().any(|c| c.is_control() && c != '\n')
    {
        return Err(ApiError::bad_request(
            "invalid_report",
            format!("reason must be 1-{MAX_REASON} characters"),
        ));
    }
    let match_id = body
        .match_id
        .as_deref()
        .map(|m| check_ident(m, "match_id"))
        .transpose()?;
    enum Target {
        Profile(String),
        LobbyAlias(String, String),
    }
    let target = match (&body.profile_id, &body.lobby_id, &body.alias) {
        (Some(p), None, None) => Target::Profile(parse_id(p, "profile_id")?),
        (None, Some(l), Some(a)) => {
            Target::LobbyAlias(check_ident(l, "lobby_id")?, check_ident(a, "alias")?)
        }
        _ => {
            return Err(ApiError::bad_request(
                "invalid_report",
                "give profile_id, or lobby_id with alias",
            ));
        }
    };
    if let Target::Profile(p) = &target
        && *p == caller.profile_id
    {
        return Err(ApiError::bad_request(
            "invalid_report",
            "you cannot report yourself",
        ));
    }
    let (now, me, account) = (
        s.now_ms(),
        caller.profile_id.clone(),
        caller.account_id.clone(),
    );
    let s2 = s.clone();
    let lobby_ref = match &target {
        Target::LobbyAlias(l, _) => Some(l.clone()),
        _ => None,
    };
    let report_id = uuid::Uuid::now_v7().hyphenated().to_string();
    let rid = report_id.clone();
    s.db.tx(move |c| {
        let reported = match target {
            Target::Profile(p) => p,
            Target::LobbyAlias(l, alias) => {
                let mine: bool = c.query_row(
                    "SELECT EXISTS(SELECT 1 FROM lobbies WHERE lobby_id = ?1 AND profile_id = ?2 AND expires_ms > ?3)",
                    params![l, me, now],
                    |r| r.get(0),
                )?;
                if !mine {
                    return Err(not_found_profile());
                }
                c.query_row(
                    "SELECT profile_id FROM lobbies WHERE lobby_id = ?1 AND alias = ?2",
                    params![l, alias],
                    |r| r.get::<_, String>(0),
                )
                .optional()?
                .ok_or_else(not_found_profile)?
            }
        };
        if reported == me {
            return Err(ApiError::bad_request("invalid_report", "you cannot report yourself"));
        }
        let p = db::profile(c, &reported)?.ok_or_else(not_found_profile)?;
        s2.limits.report.check(&me, now)?;
        c.execute(
            "INSERT INTO reports (report_id, reporter_profile, reporter_account, reported_profile, reported_account, match_id, lobby_id, reason, created_ms)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)",
            params![rid, me, account, reported, p.account_id, match_id, lobby_ref, reason, now],
        )?;
        Ok(())
    })
    .await?;
    tracing::info!(report = %report_id, "report filed");
    Ok((
        StatusCode::ACCEPTED,
        Json(json!({ "report_id": report_id })),
    )
        .into_response())
}

pub async fn internal_reports(State(s): State<AppState>, headers: HeaderMap) -> R<Json<Value>> {
    service_auth(&s, &headers)?;
    let v = s
        .db
        .call(|c| {
            let mut st = c.prepare(
                "SELECT report_id, reporter_profile, reporter_account, reported_profile, reported_account, match_id, lobby_id, reason, created_ms
                 FROM reports ORDER BY created_ms DESC, report_id DESC LIMIT 200",
            )?;
            let rows = st.query_map([], |r| {
                Ok(json!({
                    "report_id": r.get::<_, String>(0)?,
                    "reporter_profile_id": r.get::<_, String>(1)?,
                    "reporter_account_id": r.get::<_, String>(2)?,
                    "reported_profile_id": r.get::<_, String>(3)?,
                    "reported_account_id": r.get::<_, String>(4)?,
                    "match_id": r.get::<_, Option<String>>(5)?,
                    "lobby_id": r.get::<_, Option<String>>(6)?,
                    "reason": r.get::<_, String>(7)?,
                    "created_at_ms": r.get::<_, i64>(8)?,
                }))
            })?;
            Ok(rows.collect::<Result<Vec<_>, _>>()?)
        })
        .await?;
    Ok(Json(json!({ "reports": v })))
}

// ---------------------------------------------------------------- housekeeping

/// Drops expired lobbies and recent players older than the retention.
pub fn prune(c: &Connection, now: i64) -> R<()> {
    c.execute("DELETE FROM lobbies WHERE expires_ms <= ?1", [now])?;
    c.execute(
        "DELETE FROM encounters WHERE at_ms <= ?1",
        [now - RECENT_RETENTION_MS],
    )?;
    Ok(())
}
