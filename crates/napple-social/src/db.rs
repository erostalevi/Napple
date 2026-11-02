//! SQLite storage (WAL) and the JSON views built from it. One connection behind a mutex: the service is a single
//! process and every operation is a handful of indexed statements, so serialising them keeps limit checks race-free.

use std::path::Path;
use std::sync::{Arc, Mutex};

use rusqlite::{Connection, OptionalExtension, Row, params};
use serde::Serialize;

use crate::codes;
use crate::error::ApiError;

const SCHEMA_VERSION: i64 = 1;

const SCHEMA: &str = r#"
CREATE TABLE IF NOT EXISTS profiles (
    profile_id          TEXT PRIMARY KEY,
    account_id          TEXT NOT NULL,
    friend_code         TEXT NOT NULL UNIQUE,
    nickname            TEXT NOT NULL DEFAULT '',
    icon_id             TEXT NOT NULL DEFAULT '',
    presence_visibility TEXT NOT NULL DEFAULT 'friends' CHECK (presence_visibility IN ('friends', 'nobody')),
    appear_offline      INTEGER NOT NULL DEFAULT 0,
    accept_requests     INTEGER NOT NULL DEFAULT 1,
    last_seen_ms        INTEGER,
    visible_last_seen_ms INTEGER,
    created_ms          INTEGER NOT NULL,
    updated_ms          INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS profiles_account ON profiles(account_id);

CREATE TABLE IF NOT EXISTS friendships (
    profile_a  TEXT NOT NULL REFERENCES profiles(profile_id) ON DELETE CASCADE,
    profile_b  TEXT NOT NULL REFERENCES profiles(profile_id) ON DELETE CASCADE,
    created_ms INTEGER NOT NULL,
    PRIMARY KEY (profile_a, profile_b),
    CHECK (profile_a < profile_b)
);
CREATE INDEX IF NOT EXISTS friendships_b ON friendships(profile_b);

CREATE TABLE IF NOT EXISTS friend_requests (
    request_id   TEXT PRIMARY KEY,
    from_profile TEXT NOT NULL REFERENCES profiles(profile_id) ON DELETE CASCADE,
    to_profile   TEXT NOT NULL REFERENCES profiles(profile_id) ON DELETE CASCADE,
    created_ms   INTEGER NOT NULL,
    UNIQUE (from_profile, to_profile)
);
CREATE INDEX IF NOT EXISTS friend_requests_to ON friend_requests(to_profile);

CREATE TABLE IF NOT EXISTS blocks (
    blocker    TEXT NOT NULL REFERENCES profiles(profile_id) ON DELETE CASCADE,
    blocked    TEXT NOT NULL REFERENCES profiles(profile_id) ON DELETE CASCADE,
    created_ms INTEGER NOT NULL,
    PRIMARY KEY (blocker, blocked)
);
CREATE INDEX IF NOT EXISTS blocks_blocked ON blocks(blocked);

CREATE TABLE IF NOT EXISTS presence (
    profile_id        TEXT PRIMARY KEY REFERENCES profiles(profile_id) ON DELETE CASCADE,
    status            TEXT NOT NULL CHECK (status IN ('online', 'playing')),
    title_id          TEXT,
    title_name        TEXT,
    mode              TEXT,
    join_token        TEXT,
    online_since_ms   INTEGER NOT NULL,
    last_heartbeat_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS presence_heartbeat ON presence(last_heartbeat_ms);

CREATE TABLE IF NOT EXISTS invites (
    invite_id    TEXT PRIMARY KEY,
    from_profile TEXT NOT NULL REFERENCES profiles(profile_id) ON DELETE CASCADE,
    to_profile   TEXT NOT NULL REFERENCES profiles(profile_id) ON DELETE CASCADE,
    join_token   TEXT NOT NULL,
    title_id     TEXT,
    title_name   TEXT,
    mode         TEXT,
    message      TEXT,
    created_ms   INTEGER NOT NULL,
    expires_ms   INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS invites_to ON invites(to_profile);
CREATE INDEX IF NOT EXISTS invites_from ON invites(from_profile);
CREATE INDEX IF NOT EXISTS invites_expires ON invites(expires_ms);
"#;

#[derive(Clone)]
pub struct Db {
    conn: Arc<Mutex<Connection>>,
}

impl Db {
    pub fn open(path: &Path) -> rusqlite::Result<Db> {
        let conn = Connection::open(path)?;
        let mode: String = conn.query_row("PRAGMA journal_mode=WAL", [], |r| r.get(0))?;
        if !mode.eq_ignore_ascii_case("wal") {
            tracing::warn!(%mode, "sqlite did not switch to WAL");
        }
        conn.execute_batch("PRAGMA synchronous=NORMAL;")?;
        Self::init(conn)
    }

    pub fn open_in_memory() -> rusqlite::Result<Db> {
        Self::init(Connection::open_in_memory()?)
    }

    fn init(conn: Connection) -> rusqlite::Result<Db> {
        conn.busy_timeout(std::time::Duration::from_secs(5))?;
        conn.execute_batch("PRAGMA foreign_keys=ON;")?;
        let v: i64 = conn.query_row("PRAGMA user_version", [], |r| r.get(0))?;
        if v > SCHEMA_VERSION {
            return Err(rusqlite::Error::InvalidParameterName(format!("database schema {v} is newer than this build ({SCHEMA_VERSION})")));
        }
        conn.execute_batch(SCHEMA)?;
        conn.execute_batch(&format!("PRAGMA user_version={SCHEMA_VERSION};"))?;
        Ok(Db { conn: Arc::new(Mutex::new(conn)) })
    }

    /// Runs `f` on the connection in a blocking thread.
    pub async fn call<F, R>(&self, f: F) -> Result<R, ApiError>
    where
        F: FnOnce(&mut Connection) -> Result<R, ApiError> + Send + 'static,
        R: Send + 'static,
    {
        let conn = self.conn.clone();
        tokio::task::spawn_blocking(move || {
            let mut c = conn.lock().unwrap_or_else(|e| e.into_inner());
            f(&mut c)
        })
        .await
        .map_err(|e| ApiError::internal(format!("db task: {e}")))?
    }

    /// Same, inside one transaction (committed on Ok, rolled back on Err).
    pub async fn tx<F, R>(&self, f: F) -> Result<R, ApiError>
    where
        F: FnOnce(&Connection) -> Result<R, ApiError> + Send + 'static,
        R: Send + 'static,
    {
        self.call(move |c| {
            let tx = c.transaction()?;
            let r = f(&tx)?;
            tx.commit()?;
            Ok(r)
        })
        .await
    }
}

// ---------------------------------------------------------------- rows

#[derive(Debug, Clone)]
pub struct ProfileRow {
    pub profile_id: String,
    pub account_id: String,
    pub friend_code: String,
    pub nickname: String,
    pub icon_id: String,
    pub presence_visibility: String,
    pub appear_offline: bool,
    pub accept_requests: bool,
    pub last_seen_ms: Option<i64>,
    pub visible_last_seen_ms: Option<i64>,
}

const PROFILE_COLS: &str = "profile_id, account_id, friend_code, nickname, icon_id, presence_visibility, appear_offline, accept_requests, last_seen_ms, visible_last_seen_ms";

fn profile_from(r: &Row) -> rusqlite::Result<ProfileRow> {
    Ok(ProfileRow {
        profile_id: r.get(0)?,
        account_id: r.get(1)?,
        friend_code: r.get(2)?,
        nickname: r.get(3)?,
        icon_id: r.get(4)?,
        presence_visibility: r.get(5)?,
        appear_offline: r.get(6)?,
        accept_requests: r.get(7)?,
        last_seen_ms: r.get(8)?,
        visible_last_seen_ms: r.get(9)?,
    })
}

impl ProfileRow {
    /// Presence can be shown to friends at all.
    pub fn visible(&self) -> bool {
        self.presence_visibility == "friends" && !self.appear_offline
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PresenceRow {
    pub status: String,
    pub title_id: Option<String>,
    pub title_name: Option<String>,
    pub mode: Option<String>,
    pub join_token: Option<String>,
    pub online_since_ms: i64,
    pub last_heartbeat_ms: i64,
}

#[derive(Debug, Clone)]
pub struct RequestRow {
    pub request_id: String,
    pub from_profile: String,
    pub to_profile: String,
    pub created_ms: i64,
}

#[derive(Debug, Clone)]
pub struct InviteRow {
    pub invite_id: String,
    pub from_profile: String,
    pub to_profile: String,
    pub join_token: String,
    pub title_id: Option<String>,
    pub title_name: Option<String>,
    pub mode: Option<String>,
    pub message: Option<String>,
    pub created_ms: i64,
    pub expires_ms: i64,
}

// ---------------------------------------------------------------- views (JSON)

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct ProfileView {
    pub profile_id: String,
    pub nickname: String,
    pub icon_id: String,
}

impl From<&ProfileRow> for ProfileView {
    fn from(p: &ProfileRow) -> Self {
        ProfileView { profile_id: p.profile_id.clone(), nickname: p.nickname.clone(), icon_id: p.icon_id.clone() }
    }
}

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
pub struct PresenceView {
    /// "online", "playing" or "offline".
    pub status: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub title_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub title_name: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub mode: Option<String>,
    /// True when `join_token` is set: the friend is in a session others may join.
    pub joinable: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub join_token: Option<String>,
    /// Unix ms of the last heartbeat friends may know about; null when hidden or never seen.
    pub last_seen_ms: Option<i64>,
}

impl PresenceView {
    pub fn offline(last_seen_ms: Option<i64>) -> Self {
        PresenceView { status: "offline".into(), title_id: None, title_name: None, mode: None, joinable: false, join_token: None, last_seen_ms }
    }

    /// Same live state, ignoring the heartbeat timestamp (used to decide whether friends need an update).
    pub fn same_state(&self, other: &PresenceView) -> bool {
        self.status == other.status
            && self.title_id == other.title_id
            && self.title_name == other.title_name
            && self.mode == other.mode
            && self.join_token == other.join_token
    }
}

fn live_view(p: &PresenceRow) -> PresenceView {
    PresenceView {
        status: p.status.clone(),
        title_id: p.title_id.clone(),
        title_name: p.title_name.clone(),
        mode: p.mode.clone(),
        joinable: p.join_token.is_some(),
        join_token: p.join_token.clone(),
        last_seen_ms: Some(p.last_heartbeat_ms),
    }
}

/// The profile's real presence (what its own devices see).
pub fn own_presence(profile: &ProfileRow, presence: Option<&PresenceRow>, now_ms: i64, timeout_ms: i64) -> PresenceView {
    match presence {
        Some(p) if now_ms - p.last_heartbeat_ms <= timeout_ms => live_view(p),
        _ => PresenceView::offline(profile.last_seen_ms),
    }
}

/// What a friend sees. Privacy: "nobody" hides everything including last seen; "appear offline" shows offline with the
/// last time the profile was visibly online. Callers only use this for friends; non-friends never see presence.
pub fn friend_presence(profile: &ProfileRow, presence: Option<&PresenceRow>, now_ms: i64, timeout_ms: i64) -> PresenceView {
    if profile.presence_visibility != "friends" {
        return PresenceView::offline(None);
    }
    if profile.appear_offline {
        return PresenceView::offline(profile.visible_last_seen_ms);
    }
    match presence {
        Some(p) if now_ms - p.last_heartbeat_ms <= timeout_ms => live_view(p),
        _ => PresenceView::offline(profile.visible_last_seen_ms),
    }
}

#[derive(Debug, Clone, Serialize)]
pub struct FriendView {
    pub profile: ProfileView,
    pub friends_since_ms: i64,
    pub presence: PresenceView,
}

#[derive(Debug, Clone, Serialize)]
pub struct RequestView {
    pub request_id: String,
    pub from: ProfileView,
    pub to: ProfileView,
    pub created_at_ms: i64,
}

#[derive(Debug, Clone, Serialize)]
pub struct InviteView {
    pub invite_id: String,
    pub from: ProfileView,
    pub to_profile_id: String,
    pub join_token: String,
    pub title_id: Option<String>,
    pub title_name: Option<String>,
    pub mode: Option<String>,
    pub message: Option<String>,
    pub created_at_ms: i64,
    pub expires_at_ms: i64,
}

#[derive(Debug, Clone, Serialize)]
pub struct BlockView {
    pub profile: ProfileView,
    pub blocked_at_ms: i64,
}

// ---------------------------------------------------------------- profiles

pub fn profile(c: &Connection, id: &str) -> Result<Option<ProfileRow>, ApiError> {
    Ok(c.query_row(&format!("SELECT {PROFILE_COLS} FROM profiles WHERE profile_id = ?1"), [id], profile_from).optional()?)
}

pub fn profile_by_code(c: &Connection, code: &str) -> Result<Option<ProfileRow>, ApiError> {
    Ok(c.query_row(&format!("SELECT {PROFILE_COLS} FROM profiles WHERE friend_code = ?1"), [code], profile_from).optional()?)
}

pub fn must_profile(c: &Connection, id: &str) -> Result<ProfileRow, ApiError> {
    profile(c, id)?.ok_or_else(|| ApiError::not_found("profile_not_found", "no such Napple profile"))
}

fn unused_code(c: &Connection) -> Result<String, ApiError> {
    for _ in 0..50 {
        let code = codes::generate();
        let taken: bool = c.query_row("SELECT EXISTS(SELECT 1 FROM profiles WHERE friend_code = ?1)", [&code], |r| r.get(0))?;
        if !taken {
            return Ok(code);
        }
    }
    Err(ApiError::internal("could not find a free friend code"))
}

/// Registers the profile on first use (assigning a friend code) and refreshes its nickname/icon snapshot.
pub fn ensure_profile(c: &Connection, account_id: &str, profile_id: &str, nickname: &str, icon_id: &str, now_ms: i64) -> Result<ProfileRow, ApiError> {
    if let Some(p) = profile(c, profile_id)? {
        if p.nickname != nickname || p.icon_id != icon_id || p.account_id != account_id {
            c.execute(
                "UPDATE profiles SET nickname = ?2, icon_id = ?3, account_id = ?4, updated_ms = ?5 WHERE profile_id = ?1",
                params![profile_id, nickname, icon_id, account_id, now_ms],
            )?;
            return must_profile(c, profile_id);
        }
        return Ok(p);
    }
    let code = unused_code(c)?;
    c.execute(
        "INSERT INTO profiles (profile_id, account_id, friend_code, nickname, icon_id, created_ms, updated_ms) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?6)",
        params![profile_id, account_id, code, nickname, icon_id, now_ms],
    )?;
    tracing::info!(profile = profile_id, "profile registered");
    must_profile(c, profile_id)
}

pub fn regenerate_code(c: &Connection, profile_id: &str, now_ms: i64) -> Result<String, ApiError> {
    let code = unused_code(c)?;
    c.execute("UPDATE profiles SET friend_code = ?2, updated_ms = ?3 WHERE profile_id = ?1", params![profile_id, code, now_ms])?;
    Ok(code)
}

pub fn update_settings(c: &Connection, profile_id: &str, visibility: Option<&str>, appear_offline: Option<bool>, accept_requests: Option<bool>, now_ms: i64) -> Result<(), ApiError> {
    c.execute(
        "UPDATE profiles SET presence_visibility = COALESCE(?2, presence_visibility), appear_offline = COALESCE(?3, appear_offline),
         accept_requests = COALESCE(?4, accept_requests), updated_ms = ?5 WHERE profile_id = ?1",
        params![profile_id, visibility, appear_offline, accept_requests, now_ms],
    )?;
    Ok(())
}

pub fn set_last_seen(c: &Connection, profile_id: &str, at_ms: i64, visible: bool) -> Result<(), ApiError> {
    if visible {
        c.execute("UPDATE profiles SET last_seen_ms = ?2, visible_last_seen_ms = ?2 WHERE profile_id = ?1", params![profile_id, at_ms])?;
    } else {
        c.execute("UPDATE profiles SET last_seen_ms = ?2 WHERE profile_id = ?1", params![profile_id, at_ms])?;
    }
    Ok(())
}

/// Removes a profile and everything that points at it (cascades).
pub fn delete_profile(c: &Connection, profile_id: &str) -> Result<(), ApiError> {
    c.execute("DELETE FROM profiles WHERE profile_id = ?1", [profile_id])?;
    Ok(())
}

// ---------------------------------------------------------------- friendships

fn ordered<'a>(a: &'a str, b: &'a str) -> (&'a str, &'a str) {
    if a < b { (a, b) } else { (b, a) }
}

pub fn friends_since(c: &Connection, a: &str, b: &str) -> Result<Option<i64>, ApiError> {
    let (x, y) = ordered(a, b);
    Ok(c.query_row("SELECT created_ms FROM friendships WHERE profile_a = ?1 AND profile_b = ?2", [x, y], |r| r.get(0)).optional()?)
}

pub fn are_friends(c: &Connection, a: &str, b: &str) -> Result<bool, ApiError> {
    Ok(friends_since(c, a, b)?.is_some())
}

pub fn friend_count(c: &Connection, p: &str) -> Result<i64, ApiError> {
    Ok(c.query_row("SELECT COUNT(*) FROM friendships WHERE profile_a = ?1 OR profile_b = ?1", [p], |r| r.get(0))?)
}

pub fn add_friendship(c: &Connection, a: &str, b: &str, now_ms: i64) -> Result<(), ApiError> {
    let (x, y) = ordered(a, b);
    c.execute("INSERT OR IGNORE INTO friendships (profile_a, profile_b, created_ms) VALUES (?1, ?2, ?3)", params![x, y, now_ms])?;
    Ok(())
}

pub fn remove_friendship(c: &Connection, a: &str, b: &str) -> Result<bool, ApiError> {
    let (x, y) = ordered(a, b);
    Ok(c.execute("DELETE FROM friendships WHERE profile_a = ?1 AND profile_b = ?2", [x, y])? > 0)
}

/// `(friend_id, since_ms)` for every friend.
pub fn friend_ids(c: &Connection, p: &str) -> Result<Vec<(String, i64)>, ApiError> {
    let mut st = c.prepare_cached(
        "SELECT profile_b, created_ms FROM friendships WHERE profile_a = ?1 UNION ALL SELECT profile_a, created_ms FROM friendships WHERE profile_b = ?1",
    )?;
    let rows = st.query_map([p], |r| Ok((r.get(0)?, r.get(1)?)))?.collect::<Result<Vec<_>, _>>()?;
    Ok(rows)
}

// ---------------------------------------------------------------- blocks

pub fn is_blocked(c: &Connection, blocker: &str, blocked: &str) -> Result<bool, ApiError> {
    Ok(c.query_row("SELECT EXISTS(SELECT 1 FROM blocks WHERE blocker = ?1 AND blocked = ?2)", [blocker, blocked], |r| r.get(0))?)
}

pub fn block_count(c: &Connection, p: &str) -> Result<i64, ApiError> {
    Ok(c.query_row("SELECT COUNT(*) FROM blocks WHERE blocker = ?1", [p], |r| r.get(0))?)
}

pub fn add_block(c: &Connection, blocker: &str, blocked: &str, now_ms: i64) -> Result<(), ApiError> {
    c.execute("INSERT OR IGNORE INTO blocks (blocker, blocked, created_ms) VALUES (?1, ?2, ?3)", params![blocker, blocked, now_ms])?;
    Ok(())
}

pub fn remove_block(c: &Connection, blocker: &str, blocked: &str) -> Result<bool, ApiError> {
    Ok(c.execute("DELETE FROM blocks WHERE blocker = ?1 AND blocked = ?2", [blocker, blocked])? > 0)
}

pub fn blocks(c: &Connection, p: &str) -> Result<Vec<BlockView>, ApiError> {
    let mut st = c.prepare_cached(&format!(
        "SELECT {}, b.created_ms FROM blocks b JOIN profiles p ON p.profile_id = b.blocked WHERE b.blocker = ?1 ORDER BY b.created_ms",
        PROFILE_COLS.split(", ").map(|c| format!("p.{c}")).collect::<Vec<_>>().join(", ")
    ))?;
    let rows = st
        .query_map([p], |r| Ok(BlockView { profile: ProfileView::from(&profile_from(r)?), blocked_at_ms: r.get(10)? }))?
        .collect::<Result<Vec<_>, _>>()?;
    Ok(rows)
}

// ---------------------------------------------------------------- friend requests

fn request_from(r: &Row) -> rusqlite::Result<RequestRow> {
    Ok(RequestRow { request_id: r.get(0)?, from_profile: r.get(1)?, to_profile: r.get(2)?, created_ms: r.get(3)? })
}

pub fn request(c: &Connection, id: &str) -> Result<Option<RequestRow>, ApiError> {
    Ok(c.query_row("SELECT request_id, from_profile, to_profile, created_ms FROM friend_requests WHERE request_id = ?1", [id], request_from).optional()?)
}

pub fn request_between(c: &Connection, from: &str, to: &str) -> Result<Option<RequestRow>, ApiError> {
    Ok(c
        .query_row(
            "SELECT request_id, from_profile, to_profile, created_ms FROM friend_requests WHERE from_profile = ?1 AND to_profile = ?2",
            [from, to],
            request_from,
        )
        .optional()?)
}

pub fn insert_request(c: &Connection, id: &str, from: &str, to: &str, now_ms: i64) -> Result<(), ApiError> {
    c.execute("INSERT INTO friend_requests (request_id, from_profile, to_profile, created_ms) VALUES (?1, ?2, ?3, ?4)", params![id, from, to, now_ms])?;
    Ok(())
}

pub fn delete_request(c: &Connection, id: &str) -> Result<(), ApiError> {
    c.execute("DELETE FROM friend_requests WHERE request_id = ?1", [id])?;
    Ok(())
}

pub fn outgoing_request_count(c: &Connection, p: &str) -> Result<i64, ApiError> {
    Ok(c.query_row("SELECT COUNT(*) FROM friend_requests WHERE from_profile = ?1", [p], |r| r.get(0))?)
}

pub fn incoming_request_count(c: &Connection, p: &str) -> Result<i64, ApiError> {
    Ok(c.query_row("SELECT COUNT(*) FROM friend_requests WHERE to_profile = ?1", [p], |r| r.get(0))?)
}

/// Requests in both directions between two profiles.
pub fn requests_between_pair(c: &Connection, a: &str, b: &str) -> Result<Vec<RequestRow>, ApiError> {
    let mut v = Vec::new();
    v.extend(request_between(c, a, b)?);
    v.extend(request_between(c, b, a)?);
    Ok(v)
}

/// Requests where `p` is the sender (`outgoing`) or the receiver.
pub fn requests_for(c: &Connection, p: &str, outgoing: bool) -> Result<Vec<RequestRow>, ApiError> {
    let sql = if outgoing {
        "SELECT request_id, from_profile, to_profile, created_ms FROM friend_requests WHERE from_profile = ?1 ORDER BY created_ms"
    } else {
        "SELECT request_id, from_profile, to_profile, created_ms FROM friend_requests WHERE to_profile = ?1 ORDER BY created_ms"
    };
    let mut st = c.prepare_cached(sql)?;
    let rows = st.query_map([p], request_from)?.collect::<Result<Vec<_>, _>>()?;
    Ok(rows)
}

pub fn request_view(c: &Connection, r: &RequestRow) -> Result<RequestView, ApiError> {
    let from = must_profile(c, &r.from_profile)?;
    let to = must_profile(c, &r.to_profile)?;
    Ok(RequestView { request_id: r.request_id.clone(), from: (&from).into(), to: (&to).into(), created_at_ms: r.created_ms })
}

// ---------------------------------------------------------------- presence

pub fn presence(c: &Connection, p: &str) -> Result<Option<PresenceRow>, ApiError> {
    Ok(c
        .query_row(
            "SELECT status, title_id, title_name, mode, join_token, online_since_ms, last_heartbeat_ms FROM presence WHERE profile_id = ?1",
            [p],
            |r| {
                Ok(PresenceRow {
                    status: r.get(0)?,
                    title_id: r.get(1)?,
                    title_name: r.get(2)?,
                    mode: r.get(3)?,
                    join_token: r.get(4)?,
                    online_since_ms: r.get(5)?,
                    last_heartbeat_ms: r.get(6)?,
                })
            },
        )
        .optional()?)
}

pub fn upsert_presence(c: &Connection, p: &str, row: &PresenceRow) -> Result<(), ApiError> {
    c.execute(
        "INSERT INTO presence (profile_id, status, title_id, title_name, mode, join_token, online_since_ms, last_heartbeat_ms)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)
         ON CONFLICT(profile_id) DO UPDATE SET status = excluded.status, title_id = excluded.title_id, title_name = excluded.title_name,
           mode = excluded.mode, join_token = excluded.join_token, online_since_ms = excluded.online_since_ms, last_heartbeat_ms = excluded.last_heartbeat_ms",
        params![p, row.status, row.title_id, row.title_name, row.mode, row.join_token, row.online_since_ms, row.last_heartbeat_ms],
    )?;
    Ok(())
}

pub fn delete_presence(c: &Connection, p: &str) -> Result<(), ApiError> {
    c.execute("DELETE FROM presence WHERE profile_id = ?1", [p])?;
    Ok(())
}

/// Profiles whose last heartbeat is older than the timeout.
pub fn expired_presence(c: &Connection, now_ms: i64, timeout_ms: i64) -> Result<Vec<String>, ApiError> {
    let mut st = c.prepare_cached("SELECT profile_id FROM presence WHERE last_heartbeat_ms < ?1")?;
    let rows = st.query_map([now_ms - timeout_ms], |r| r.get(0))?.collect::<Result<Vec<_>, _>>()?;
    Ok(rows)
}

// ---------------------------------------------------------------- invites

const INVITE_COLS: &str = "invite_id, from_profile, to_profile, join_token, title_id, title_name, mode, message, created_ms, expires_ms";

fn invite_from(r: &Row) -> rusqlite::Result<InviteRow> {
    Ok(InviteRow {
        invite_id: r.get(0)?,
        from_profile: r.get(1)?,
        to_profile: r.get(2)?,
        join_token: r.get(3)?,
        title_id: r.get(4)?,
        title_name: r.get(5)?,
        mode: r.get(6)?,
        message: r.get(7)?,
        created_ms: r.get(8)?,
        expires_ms: r.get(9)?,
    })
}

pub fn invite(c: &Connection, id: &str) -> Result<Option<InviteRow>, ApiError> {
    Ok(c.query_row(&format!("SELECT {INVITE_COLS} FROM invites WHERE invite_id = ?1"), [id], invite_from).optional()?)
}

pub fn insert_invite(c: &Connection, i: &InviteRow) -> Result<(), ApiError> {
    c.execute(
        &format!("INSERT INTO invites ({INVITE_COLS}) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)"),
        params![i.invite_id, i.from_profile, i.to_profile, i.join_token, i.title_id, i.title_name, i.mode, i.message, i.created_ms, i.expires_ms],
    )?;
    Ok(())
}

pub fn delete_invite(c: &Connection, id: &str) -> Result<(), ApiError> {
    c.execute("DELETE FROM invites WHERE invite_id = ?1", [id])?;
    Ok(())
}

/// Live invites sent (`outgoing`) or received by `p`.
pub fn invites_for(c: &Connection, p: &str, outgoing: bool, now_ms: i64) -> Result<Vec<InviteRow>, ApiError> {
    let col = if outgoing { "from_profile" } else { "to_profile" };
    let mut st = c.prepare_cached(&format!("SELECT {INVITE_COLS} FROM invites WHERE {col} = ?1 AND expires_ms > ?2 ORDER BY created_ms"))?;
    let rows = st.query_map(params![p, now_ms], invite_from)?.collect::<Result<Vec<_>, _>>()?;
    Ok(rows)
}

pub fn outgoing_invite_count(c: &Connection, p: &str, now_ms: i64) -> Result<i64, ApiError> {
    Ok(c.query_row("SELECT COUNT(*) FROM invites WHERE from_profile = ?1 AND expires_ms > ?2", params![p, now_ms], |r| r.get(0))?)
}

/// Invites in either direction between two profiles (live or not).
pub fn invites_between_pair(c: &Connection, a: &str, b: &str) -> Result<Vec<InviteRow>, ApiError> {
    let mut st = c.prepare_cached(&format!(
        "SELECT {INVITE_COLS} FROM invites WHERE (from_profile = ?1 AND to_profile = ?2) OR (from_profile = ?2 AND to_profile = ?1)"
    ))?;
    let rows = st.query_map([a, b], invite_from)?.collect::<Result<Vec<_>, _>>()?;
    Ok(rows)
}

pub fn expired_invites(c: &Connection, now_ms: i64) -> Result<Vec<InviteRow>, ApiError> {
    let mut st = c.prepare_cached(&format!("SELECT {INVITE_COLS} FROM invites WHERE expires_ms <= ?1"))?;
    let rows = st.query_map([now_ms], invite_from)?.collect::<Result<Vec<_>, _>>()?;
    Ok(rows)
}

pub fn invite_view(c: &Connection, i: &InviteRow) -> Result<InviteView, ApiError> {
    let from = must_profile(c, &i.from_profile)?;
    Ok(InviteView {
        invite_id: i.invite_id.clone(),
        from: (&from).into(),
        to_profile_id: i.to_profile.clone(),
        join_token: i.join_token.clone(),
        title_id: i.title_id.clone(),
        title_name: i.title_name.clone(),
        mode: i.mode.clone(),
        message: i.message.clone(),
        created_at_ms: i.created_ms,
        expires_at_ms: i.expires_ms,
    })
}

/// Every profile's friend view, as `viewer` sees them.
pub fn friend_views(c: &Connection, viewer: &str, now_ms: i64, timeout_ms: i64) -> Result<Vec<FriendView>, ApiError> {
    let mut out = Vec::new();
    for (fid, since) in friend_ids(c, viewer)? {
        out.push(friend_view(c, &fid, since, now_ms, timeout_ms)?);
    }
    // Online first, then by nickname.
    out.sort_by(|a, b| {
        (a.presence.status == "offline", a.profile.nickname.to_lowercase(), a.profile.profile_id.clone()).cmp(&(
            b.presence.status == "offline",
            b.profile.nickname.to_lowercase(),
            b.profile.profile_id.clone(),
        ))
    });
    Ok(out)
}

pub fn friend_view(c: &Connection, friend: &str, since_ms: i64, now_ms: i64, timeout_ms: i64) -> Result<FriendView, ApiError> {
    let p = must_profile(c, friend)?;
    let pr = presence(c, friend)?;
    Ok(FriendView { profile: (&p).into(), friends_since_ms: since_ms, presence: friend_presence(&p, pr.as_ref(), now_ms, timeout_ms) })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn schema_and_codes_unique() {
        let db = Db::open_in_memory().unwrap();
        let c = db.conn.lock().unwrap();
        let mut seen = std::collections::HashSet::new();
        for i in 0..200 {
            let p = ensure_profile(&c, "acc", &format!("p{i:03}"), "n", "1", 1).unwrap();
            assert!(seen.insert(p.friend_code.clone()));
            assert!(codes::parse(&p.friend_code).is_some());
        }
        // Re-ensuring keeps the code and refreshes the snapshot.
        let before = profile(&c, "p000").unwrap().unwrap();
        let after = ensure_profile(&c, "acc", "p000", "renamed", "9", 2).unwrap();
        assert_eq!(before.friend_code, after.friend_code);
        assert_eq!(after.nickname, "renamed");
        // Friendship ordering and cascade.
        add_friendship(&c, "p002", "p001", 5).unwrap();
        assert!(are_friends(&c, "p001", "p002").unwrap());
        assert_eq!(friend_count(&c, "p002").unwrap(), 1);
        delete_profile(&c, "p001").unwrap();
        assert_eq!(friend_count(&c, "p002").unwrap(), 0);
    }

    #[test]
    fn wal_on_disk() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("s.sqlite3");
        let db = Db::open(&path).unwrap();
        let c = db.conn.lock().unwrap();
        let mode: String = c.query_row("PRAGMA journal_mode", [], |r| r.get(0)).unwrap();
        assert_eq!(mode.to_lowercase(), "wal");
    }

    #[test]
    fn privacy_views() {
        let mut p = ProfileRow {
            profile_id: "p".into(),
            account_id: "a".into(),
            friend_code: "1".into(),
            nickname: "n".into(),
            icon_id: "".into(),
            presence_visibility: "friends".into(),
            appear_offline: false,
            accept_requests: true,
            last_seen_ms: Some(900),
            visible_last_seen_ms: Some(500),
        };
        let pr = PresenceRow {
            status: "playing".into(),
            title_id: Some("0100000000010000".into()),
            title_name: Some("Game".into()),
            mode: None,
            join_token: Some("tok".into()),
            online_since_ms: 0,
            last_heartbeat_ms: 1000,
        };
        assert_eq!(friend_presence(&p, Some(&pr), 1000, 180_000).status, "playing");
        assert!(friend_presence(&p, Some(&pr), 1000, 180_000).joinable);
        assert_eq!(friend_presence(&p, Some(&pr), 182_000, 180_000).status, "offline");
        p.appear_offline = true;
        let v = friend_presence(&p, Some(&pr), 1000, 180_000);
        assert_eq!((v.status.as_str(), v.last_seen_ms, v.join_token), ("offline", Some(500), None));
        assert_eq!(own_presence(&p, Some(&pr), 1000, 180_000).status, "playing");
        p.appear_offline = false;
        p.presence_visibility = "nobody".into();
        assert_eq!(friend_presence(&p, Some(&pr), 1000, 180_000), PresenceView::offline(None));
    }
}
