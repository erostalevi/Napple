//! REST handlers under `/social/v1`. Every handler takes a [`Caller`] (verified token + owned profile).
//! Writes run in one SQLite transaction; WebSocket events go out after the commit.

use axum::Json;
use axum::extract::{FromRequest, FromRequestParts, State};
use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

use crate::config::{
    BLOCK_LIMIT, FRIEND_LIMIT, INCOMING_REQUEST_LIMIT, INVITE_TTL_MS, MAX_INVITE_MESSAGE,
    MAX_JOIN_TOKEN, MAX_MODE, MAX_TITLE_NAME, OUTGOING_INVITE_LIMIT, OUTGOING_REQUEST_LIMIT,
};
use crate::db::{self, FriendView, InviteRow, PresenceRow, PresenceView, ProfileRow, RequestRow};
use crate::error::ApiError;
use crate::hub::Event;
use crate::names;
use crate::{AppState, Caller, PresenceChange, codes, presence_pair};

type R<T> = Result<T, ApiError>;

/// JSON body whose errors come back in the API error format.
#[derive(FromRequest)]
#[from_request(via(axum::Json), rejection(ApiError))]
pub struct ApiJson<T>(pub T);

/// Path parameters whose errors come back in the API error format.
#[derive(FromRequestParts)]
#[from_request(via(axum::extract::Path), rejection(ApiError))]
pub struct ApiPath<T>(pub T);

fn uuid_param(s: &str, what: &'static str) -> R<String> {
    uuid::Uuid::parse_str(s.trim())
        .map(|u| u.hyphenated().to_string())
        .map_err(|_| ApiError::bad_request("invalid_id", format!("{what} must be a UUID")))
}

fn no_content() -> Response {
    StatusCode::NO_CONTENT.into_response()
}

fn new_id() -> String {
    uuid::Uuid::now_v7().hyphenated().to_string()
}

// ---------------------------------------------------------------- me / settings

#[derive(Serialize)]
struct Settings {
    presence_visibility: String,
    appear_offline: bool,
    accept_friend_requests: bool,
}

fn settings_of(p: &ProfileRow) -> Settings {
    Settings {
        presence_visibility: p.presence_visibility.clone(),
        appear_offline: p.appear_offline,
        accept_friend_requests: p.accept_requests,
    }
}

pub async fn me(State(s): State<AppState>, caller: Caller) -> R<Json<Value>> {
    let (now, timeout) = (s.now_ms(), s.timeout_ms());
    let device = caller.device_id.clone();
    let v =
        s.db.call(move |c| {
            let p = db::must_profile(c, &caller.profile_id)?;
            let pr = db::presence(c, &caller.profile_id)?;
            Ok(json!({
                "profile_id": p.profile_id,
                "account_id": p.account_id,
                "device_id": device,
                "nickname": p.nickname,
                "icon_id": p.icon_id,
                "friend_code": codes::display(&p.friend_code),
                "settings": settings_of(&p),
                "friend_count": db::friend_count(c, &p.profile_id)?,
                "friend_limit": FRIEND_LIMIT,
                "presence": db::own_presence(&p, pr.as_ref(), now, timeout),
            }))
        })
        .await?;
    Ok(Json(v))
}

/// Erases this profile's social data (friends, requests, blocks, invites, presence). The next authenticated call
/// registers the profile again with a new friend code. The account service should call this when a profile is deleted.
pub async fn delete_me(State(s): State<AppState>, caller: Caller) -> R<Response> {
    let me = caller.profile_id.clone();
    let (friends, requests, invites) =
        s.db.tx(move |c| {
            let friends: Vec<String> = db::friend_ids(c, &me)?
                .into_iter()
                .map(|(f, _)| f)
                .collect();
            let mut requests = db::requests_for(c, &me, true)?;
            requests.extend(db::requests_for(c, &me, false)?);
            let mut invites = db::invites_for(c, &me, true, i64::MIN)?;
            invites.extend(db::invites_for(c, &me, false, i64::MIN)?);
            db::delete_profile(c, &me)?;
            Ok((friends, requests, invites))
        })
        .await?;
    let me = &caller.profile_id;
    s.hub.send_many(
        &friends,
        &Event::FriendRemoved {
            profile_id: me.clone(),
        },
    );
    for r in requests {
        let other = if &r.from_profile == me {
            r.to_profile
        } else {
            r.from_profile
        };
        s.hub.send(
            &other,
            &Event::FriendRequestClosed {
                request_id: r.request_id,
            },
        );
    }
    for i in invites {
        let other = if &i.from_profile == me {
            i.to_profile
        } else {
            i.from_profile
        };
        s.hub.send(
            &other,
            &Event::InviteClosed {
                invite_id: i.invite_id,
                reason: "removed".into(),
            },
        );
    }
    tracing::info!(profile = %me, "profile social data erased");
    Ok(no_content())
}

pub async fn regenerate_code(State(s): State<AppState>, caller: Caller) -> R<Json<Value>> {
    let now = s.now_ms();
    s.limits.code_regen.check(&caller.profile_id, now)?;
    let code =
        s.db.tx(move |c| db::regenerate_code(c, &caller.profile_id, now))
            .await?;
    Ok(Json(json!({ "friend_code": codes::display(&code) })))
}

pub async fn get_settings(State(s): State<AppState>, caller: Caller) -> R<Json<Value>> {
    let p =
        s.db.call(move |c| db::must_profile(c, &caller.profile_id))
            .await?;
    Ok(Json(json!(settings_of(&p))))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SettingsBody {
    presence_visibility: Option<String>,
    appear_offline: Option<bool>,
    accept_friend_requests: Option<bool>,
}

pub async fn put_settings(
    State(s): State<AppState>,
    caller: Caller,
    ApiJson(body): ApiJson<SettingsBody>,
) -> R<Json<Value>> {
    if let Some(v) = &body.presence_visibility
        && v != "friends"
        && v != "nobody"
    {
        return Err(ApiError::bad_request(
            "invalid_setting",
            "presence_visibility must be \"friends\" or \"nobody\"",
        ));
    }
    let (now, timeout) = (s.now_ms(), s.timeout_ms());
    let pid = caller.profile_id.clone();
    let (change, p) =
        s.db.tx(move |c| {
            let (friend_before, own_before) = presence_pair(c, &pid, now, timeout)?;
            db::update_settings(
                c,
                &pid,
                body.presence_visibility.as_deref(),
                body.appear_offline,
                body.accept_friend_requests,
                now,
            )?;
            let (friend_after, own_after) = presence_pair(c, &pid, now, timeout)?;
            let friends = db::friend_ids(c, &pid)?
                .into_iter()
                .map(|(f, _)| f)
                .collect();
            let p = db::must_profile(c, &pid)?;
            Ok((
                PresenceChange {
                    profile_id: pid,
                    friends,
                    friend_before,
                    friend_after,
                    own_before,
                    own_after,
                },
                p,
            ))
        })
        .await?;
    s.push_presence(&change);
    let st = settings_of(&p);
    s.hub.send(
        &caller.profile_id,
        &Event::Settings {
            presence_visibility: st.presence_visibility.clone(),
            appear_offline: st.appear_offline,
            accept_friend_requests: st.accept_friend_requests,
        },
    );
    Ok(Json(json!(st)))
}

// ---------------------------------------------------------------- friends

pub async fn friends(State(s): State<AppState>, caller: Caller) -> R<Json<Value>> {
    let (now, timeout) = (s.now_ms(), s.timeout_ms());
    let list =
        s.db.call(move |c| db::friend_views(c, &caller.profile_id, now, timeout))
            .await?;
    Ok(Json(
        json!({ "count": list.len(), "limit": FRIEND_LIMIT, "friends": list }),
    ))
}

pub async fn remove_friend(
    State(s): State<AppState>,
    caller: Caller,
    ApiPath(other): ApiPath<String>,
) -> R<Response> {
    let other = uuid_param(&other, "profile_id")?;
    let (me, o) = (caller.profile_id.clone(), other.clone());
    let invites =
        s.db.tx(move |c| {
            if !db::remove_friendship(c, &me, &o)? {
                return Err(ApiError::not_found(
                    "not_friends",
                    "that profile is not your friend",
                ));
            }
            let invites = db::invites_between_pair(c, &me, &o)?;
            for i in &invites {
                db::delete_invite(c, &i.invite_id)?;
            }
            Ok(invites)
        })
        .await?;
    s.hub.send(
        &other,
        &Event::FriendRemoved {
            profile_id: caller.profile_id.clone(),
        },
    );
    s.hub.send(
        &caller.profile_id,
        &Event::FriendRemoved {
            profile_id: other.clone(),
        },
    );
    close_invites(&s, &invites, "removed");
    Ok(no_content())
}

fn close_invites(s: &AppState, invites: &[InviteRow], reason: &str) {
    for i in invites {
        let ev = Event::InviteClosed {
            invite_id: i.invite_id.clone(),
            reason: reason.into(),
        };
        s.hub.send(&i.to_profile, &ev);
        s.hub.send(&i.from_profile, &ev);
    }
}

// ---------------------------------------------------------------- friend requests

pub async fn list_requests(State(s): State<AppState>, caller: Caller) -> R<Json<Value>> {
    let v =
        s.db.call(move |c| {
            let incoming = db::requests_for(c, &caller.profile_id, false)?
                .iter()
                .map(|r| db::request_view(c, r))
                .collect::<R<Vec<_>>>()?;
            let outgoing = db::requests_for(c, &caller.profile_id, true)?
                .iter()
                .map(|r| db::request_view(c, r))
                .collect::<R<Vec<_>>>()?;
            Ok(json!({ "incoming": incoming, "outgoing": outgoing }))
        })
        .await?;
    Ok(Json(v))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SendRequestBody {
    friend_code: String,
}

struct Accepted {
    other: String,
    /// The new friend as I see them.
    mine: FriendView,
    /// Me as the new friend sees me.
    theirs: FriendView,
    closed: Vec<String>,
}

/// Turns a pending request into a friendship, checking both friend limits.
fn accept_core(
    c: &rusqlite::Connection,
    r: &RequestRow,
    me: &str,
    now: i64,
    timeout: i64,
) -> R<Accepted> {
    let other = if r.from_profile == me {
        r.to_profile.clone()
    } else {
        r.from_profile.clone()
    };
    if db::friend_count(c, me)? >= FRIEND_LIMIT {
        return Err(ApiError::conflict(
            "friend_limit",
            format!("you already have {FRIEND_LIMIT} friends"),
        ));
    }
    if db::friend_count(c, &other)? >= FRIEND_LIMIT {
        return Err(ApiError::conflict(
            "target_friend_limit",
            format!("that profile already has {FRIEND_LIMIT} friends"),
        ));
    }
    db::add_friendship(c, me, &other, now)?;
    let mut closed = Vec::new();
    for req in db::requests_between_pair(c, me, &other)? {
        db::delete_request(c, &req.request_id)?;
        closed.push(req.request_id);
    }
    Ok(Accepted {
        mine: db::friend_view(c, &other, now, now, timeout)?,
        theirs: db::friend_view(c, me, now, now, timeout)?,
        other,
        closed,
    })
}

fn push_accepted(s: &AppState, me: &str, a: &Accepted) {
    for id in &a.closed {
        let ev = Event::FriendRequestClosed {
            request_id: id.clone(),
        };
        s.hub.send(me, &ev);
        s.hub.send(&a.other, &ev);
    }
    s.hub.send(
        &a.other,
        &Event::FriendAdded {
            friend: a.theirs.clone(),
        },
    );
    s.hub.send(
        me,
        &Event::FriendAdded {
            friend: a.mine.clone(),
        },
    );
}

enum SendOutcome {
    Pending(db::RequestView, String),
    Friends(Box<Accepted>),
}

pub async fn send_request(
    State(s): State<AppState>,
    caller: Caller,
    ApiJson(body): ApiJson<SendRequestBody>,
) -> R<Response> {
    let code = codes::parse(&body.friend_code).ok_or_else(|| {
        ApiError::bad_request(
            "invalid_friend_code",
            "not a valid Napple friend code (NP-dddd-dddd-dddd)",
        )
    })?;
    let (now, timeout) = (s.now_ms(), s.timeout_ms());
    s.limits.friend_request.check(&caller.profile_id, now)?;
    let me = caller.profile_id.clone();
    let not_found =
        || ApiError::not_found("code_not_found", "no Napple profile has that friend code");
    let outcome =
        s.db.tx(move |c| {
            let target = db::profile_by_code(c, &code)?.ok_or_else(not_found)?;
            let t = target.profile_id.as_str();
            if t == me {
                return Err(ApiError::bad_request(
                    "cannot_friend_self",
                    "that is your own friend code",
                ));
            }
            // A profile that blocked you is indistinguishable from a wrong code.
            if db::is_blocked(c, t, &me)? {
                return Err(not_found());
            }
            if db::is_blocked(c, &me, t)? {
                return Err(ApiError::conflict(
                    "blocked_by_you",
                    "you blocked this profile; unblock it first",
                ));
            }
            if db::are_friends(c, &me, t)? {
                return Err(ApiError::conflict(
                    "already_friends",
                    "you are already friends",
                ));
            }
            // They already asked you: sending back is accepting.
            if let Some(reverse) = db::request_between(c, t, &me)? {
                return Ok(SendOutcome::Friends(Box::new(accept_core(
                    c, &reverse, &me, now, timeout,
                )?)));
            }
            if db::request_between(c, &me, t)?.is_some() {
                return Err(ApiError::conflict(
                    "request_exists",
                    "you already sent this profile a friend request",
                ));
            }
            if !target.accept_requests {
                return Err(ApiError::forbidden(
                    "not_accepting_requests",
                    "this profile is not accepting friend requests",
                ));
            }
            if db::friend_count(c, &me)? >= FRIEND_LIMIT {
                return Err(ApiError::conflict(
                    "friend_limit",
                    format!("you already have {FRIEND_LIMIT} friends"),
                ));
            }
            if db::friend_count(c, t)? >= FRIEND_LIMIT {
                return Err(ApiError::conflict(
                    "target_friend_limit",
                    format!("that profile already has {FRIEND_LIMIT} friends"),
                ));
            }
            if db::outgoing_request_count(c, &me)? >= OUTGOING_REQUEST_LIMIT {
                return Err(ApiError::conflict(
                    "too_many_pending",
                    format!("you have {OUTGOING_REQUEST_LIMIT} pending requests"),
                ));
            }
            if db::incoming_request_count(c, t)? >= INCOMING_REQUEST_LIMIT {
                return Err(ApiError::conflict(
                    "target_inbox_full",
                    "that profile has too many pending requests",
                ));
            }
            let id = new_id();
            db::insert_request(c, &id, &me, t, now)?;
            let r = db::request(c, &id)?.ok_or_else(|| ApiError::internal("request vanished"))?;
            Ok(SendOutcome::Pending(
                db::request_view(c, &r)?,
                t.to_string(),
            ))
        })
        .await?;
    match outcome {
        SendOutcome::Pending(view, target) => {
            s.hub.send(
                &target,
                &Event::FriendRequest {
                    request: view.clone(),
                },
            );
            Ok((
                StatusCode::CREATED,
                Json(json!({ "status": "pending", "request": view })),
            )
                .into_response())
        }
        SendOutcome::Friends(a) => {
            push_accepted(&s, &caller.profile_id, &a);
            Ok((
                StatusCode::OK,
                Json(json!({ "status": "friends", "friend": a.mine })),
            )
                .into_response())
        }
    }
}

pub async fn accept_request(
    State(s): State<AppState>,
    caller: Caller,
    ApiPath(id): ApiPath<String>,
) -> R<Json<Value>> {
    let (now, timeout) = (s.now_ms(), s.timeout_ms());
    let me = caller.profile_id.clone();
    let a =
        s.db.tx(move |c| {
            let r = db::request(c, &id)?
                .filter(|r| r.to_profile == me)
                .ok_or_else(|| {
                    ApiError::not_found("request_not_found", "no such incoming friend request")
                })?;
            accept_core(c, &r, &me, now, timeout)
        })
        .await?;
    push_accepted(&s, &caller.profile_id, &a);
    Ok(Json(json!({ "friend": a.mine })))
}

async fn close_request(s: &AppState, caller: &Caller, id: String, incoming: bool) -> R<Response> {
    let me = caller.profile_id.clone();
    let r =
        s.db.tx(move |c| {
            let r = db::request(c, &id)?
                .filter(|r| {
                    if incoming {
                        r.to_profile == me
                    } else {
                        r.from_profile == me
                    }
                })
                .ok_or_else(|| {
                    ApiError::not_found("request_not_found", "no such friend request")
                })?;
            db::delete_request(c, &r.request_id)?;
            Ok(r)
        })
        .await?;
    let ev = Event::FriendRequestClosed {
        request_id: r.request_id.clone(),
    };
    s.hub.send(&r.from_profile, &ev);
    s.hub.send(&r.to_profile, &ev);
    Ok(no_content())
}

pub async fn decline_request(
    State(s): State<AppState>,
    caller: Caller,
    ApiPath(id): ApiPath<String>,
) -> R<Response> {
    close_request(&s, &caller, id, true).await
}

pub async fn cancel_request(
    State(s): State<AppState>,
    caller: Caller,
    ApiPath(id): ApiPath<String>,
) -> R<Response> {
    close_request(&s, &caller, id, false).await
}

// ---------------------------------------------------------------- blocks

pub async fn list_blocks(State(s): State<AppState>, caller: Caller) -> R<Json<Value>> {
    let list =
        s.db.call(move |c| db::blocks(c, &caller.profile_id))
            .await?;
    Ok(Json(
        json!({ "count": list.len(), "limit": BLOCK_LIMIT, "blocks": list }),
    ))
}

pub async fn block(
    State(s): State<AppState>,
    caller: Caller,
    ApiPath(other): ApiPath<String>,
) -> R<Json<Value>> {
    let other = uuid_param(&other, "profile_id")?;
    if other == caller.profile_id {
        return Err(ApiError::bad_request(
            "cannot_block_self",
            "you cannot block yourself",
        ));
    }
    let now = s.now_ms();
    let (me, o) = (caller.profile_id.clone(), other.clone());
    let (was_friend, requests, invites, view) =
        s.db.tx(move |c| {
            let target = db::must_profile(c, &o)?;
            if !db::is_blocked(c, &me, &o)? && db::block_count(c, &me)? >= BLOCK_LIMIT {
                return Err(ApiError::conflict(
                    "block_limit",
                    format!("you already blocked {BLOCK_LIMIT} profiles"),
                ));
            }
            let was_friend = db::remove_friendship(c, &me, &o)?;
            let requests = db::requests_between_pair(c, &me, &o)?;
            for r in &requests {
                db::delete_request(c, &r.request_id)?;
            }
            let invites = db::invites_between_pair(c, &me, &o)?;
            for i in &invites {
                db::delete_invite(c, &i.invite_id)?;
            }
            db::add_block(c, &me, &o, now)?;
            let at: i64 = c.query_row(
                "SELECT created_ms FROM blocks WHERE blocker = ?1 AND blocked = ?2",
                [&me, &o],
                |r| r.get(0),
            )?;
            Ok((
                was_friend,
                requests,
                invites,
                db::BlockView {
                    profile: (&target).into(),
                    blocked_at_ms: at,
                },
            ))
        })
        .await?;
    if was_friend {
        s.hub.send(
            &other,
            &Event::FriendRemoved {
                profile_id: caller.profile_id.clone(),
            },
        );
        s.hub.send(
            &caller.profile_id,
            &Event::FriendRemoved {
                profile_id: other.clone(),
            },
        );
    }
    for r in requests {
        let ev = Event::FriendRequestClosed {
            request_id: r.request_id,
        };
        s.hub.send(&other, &ev);
        s.hub.send(&caller.profile_id, &ev);
    }
    close_invites(&s, &invites, "removed");
    Ok(Json(json!({ "blocked": view })))
}

pub async fn unblock(
    State(s): State<AppState>,
    caller: Caller,
    ApiPath(other): ApiPath<String>,
) -> R<Response> {
    let other = uuid_param(&other, "profile_id")?;
    let removed =
        s.db.tx(move |c| db::remove_block(c, &caller.profile_id, &other))
            .await?;
    if !removed {
        return Err(ApiError::not_found(
            "not_blocked",
            "that profile is not blocked",
        ));
    }
    Ok(no_content())
}

// ---------------------------------------------------------------- presence

fn presence_response(s: &AppState, own: &PresenceView) -> Value {
    json!({
        "presence": own,
        "heartbeat_interval_secs": s.config.heartbeat_secs,
        "offline_after_secs": s.timeout_ms() / 1000,
    })
}

pub async fn get_presence(State(s): State<AppState>, caller: Caller) -> R<Json<Value>> {
    let (now, timeout) = (s.now_ms(), s.timeout_ms());
    let (friend_view, own) =
        s.db.call(move |c| presence_pair(c, &caller.profile_id, now, timeout))
            .await?;
    let mut v = presence_response(&s, &own);
    v["as_friends_see_it"] = json!(friend_view);
    Ok(Json(v))
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PresenceBody {
    status: String,
    title_id: Option<String>,
    title_name: Option<String>,
    mode: Option<String>,
    join_token: Option<String>,
}

fn clean_text(v: Option<String>, max: usize, field: &str, code: &'static str) -> R<Option<String>> {
    let Some(v) = v else { return Ok(None) };
    let v = v.trim().to_string();
    if v.is_empty() {
        return Ok(None);
    }
    if v.chars().count() > max || v.chars().any(char::is_control) {
        return Err(ApiError::bad_request(
            code,
            format!("{field}: at most {max} characters, no control characters"),
        ));
    }
    Ok(Some(v))
}

/// Reserved-name rules (ACCOUNT-CONTRACT.md "Reserved names") on free text other players see: labels (`title_name`, `mode`)
/// get the full name rules, an invite `message` only the "napple" word and invisible characters. 422 `reserved_name`.
fn reserved_check(v: &Option<String>, max: usize, field: &str, label: bool) -> R<()> {
    let Some(t) = v else { return Ok(()) };
    let r = if label {
        names::check_name(t, max, &[]).map(|_| ())
    } else {
        names::check_text(t)
    };
    r.map_err(|e| {
        ApiError::unprocessable(
            "reserved_name",
            format!("{field}: {}", e.message().replacen("names", "text", 1)),
        )
    })
}

/// `(status, title_id, title_name, mode, join_token)`; None means offline.
type PresenceFields = (
    String,
    Option<String>,
    Option<String>,
    Option<String>,
    Option<String>,
);

fn validate_presence(b: PresenceBody) -> R<Option<PresenceFields>> {
    match b.status.as_str() {
        "offline" => Ok(None),
        "online" => Ok(Some(("online".into(), None, None, None, None))),
        "playing" => {
            let tid = b
                .title_id
                .map(|t| t.trim().to_ascii_uppercase())
                .unwrap_or_default();
            if tid.len() != 16 || !tid.bytes().all(|c| c.is_ascii_hexdigit()) {
                return Err(ApiError::bad_request(
                    "invalid_presence",
                    "playing needs title_id: 16 hex digits",
                ));
            }
            let name = clean_text(
                b.title_name,
                MAX_TITLE_NAME,
                "title_name",
                "invalid_presence",
            )?;
            let mode = clean_text(b.mode, MAX_MODE, "mode", "invalid_presence")?;
            reserved_check(&name, MAX_TITLE_NAME, "title_name", true)?;
            reserved_check(&mode, MAX_MODE, "mode", true)?;
            let jt = match b.join_token {
                Some(t) if t.is_empty() => None,
                Some(t) if t.len() > MAX_JOIN_TOKEN || t.chars().any(char::is_control) => {
                    return Err(ApiError::bad_request(
                        "invalid_presence",
                        format!(
                            "join_token: at most {MAX_JOIN_TOKEN} bytes, no control characters"
                        ),
                    ));
                }
                t => t,
            };
            Ok(Some(("playing".into(), Some(tid), name, mode, jt)))
        }
        _ => Err(ApiError::bad_request(
            "invalid_presence",
            "status must be online, playing or offline",
        )),
    }
}

pub async fn put_presence(
    State(s): State<AppState>,
    caller: Caller,
    ApiJson(body): ApiJson<PresenceBody>,
) -> R<Response> {
    let Some((status, title_id, title_name, mode, join_token)) = validate_presence(body)? else {
        return go_offline(&s, &caller).await;
    };
    let (now, timeout) = (s.now_ms(), s.timeout_ms());
    let pid = caller.profile_id.clone();
    let change =
        s.db.tx(move |c| {
            let (friend_before, own_before) = presence_pair(c, &pid, now, timeout)?;
            let existing = db::presence(c, &pid)?;
            let online_since_ms = existing
                .filter(|r| now - r.last_heartbeat_ms <= timeout)
                .map_or(now, |r| r.online_since_ms);
            db::upsert_presence(
                c,
                &pid,
                &PresenceRow {
                    status,
                    title_id,
                    title_name,
                    mode,
                    join_token,
                    online_since_ms,
                    last_heartbeat_ms: now,
                },
            )?;
            let p = db::must_profile(c, &pid)?;
            db::set_last_seen(c, &pid, now, p.visible())?;
            let (friend_after, own_after) = presence_pair(c, &pid, now, timeout)?;
            let friends = db::friend_ids(c, &pid)?
                .into_iter()
                .map(|(f, _)| f)
                .collect();
            Ok(PresenceChange {
                profile_id: pid,
                friends,
                friend_before,
                friend_after,
                own_before,
                own_after,
            })
        })
        .await?;
    s.push_presence(&change);
    Ok(Json(presence_response(&s, &change.own_after)).into_response())
}

/// Refreshes the heartbeat of a live presence. 409 `presence_expired` when there is none (the client then PUTs its state).
pub async fn do_heartbeat(s: &AppState, profile_id: &str) -> R<PresenceView> {
    let (now, timeout) = (s.now_ms(), s.timeout_ms());
    let pid = profile_id.to_string();
    s.db.tx(move |c| {
        let mut row = db::presence(c, &pid)?
            .filter(|r| now - r.last_heartbeat_ms <= timeout)
            .ok_or_else(|| {
                ApiError::conflict(
                    "presence_expired",
                    "no live presence; PUT /social/v1/presence first",
                )
            })?;
        row.last_heartbeat_ms = now;
        db::upsert_presence(c, &pid, &row)?;
        let p = db::must_profile(c, &pid)?;
        db::set_last_seen(c, &pid, now, p.visible())?;
        Ok(db::own_presence(&p, Some(&row), now, timeout))
    })
    .await
}

pub async fn heartbeat(State(s): State<AppState>, caller: Caller) -> R<Json<Value>> {
    let own = do_heartbeat(&s, &caller.profile_id).await?;
    Ok(Json(presence_response(&s, &own)))
}

async fn go_offline(s: &AppState, caller: &Caller) -> R<Response> {
    let (now, timeout) = (s.now_ms(), s.timeout_ms());
    let pid = caller.profile_id.clone();
    let change =
        s.db.tx(move |c| {
            let (friend_before, own_before) = presence_pair(c, &pid, now, timeout)?;
            if own_before.status != "offline" {
                let p = db::must_profile(c, &pid)?;
                db::set_last_seen(c, &pid, now, p.visible())?;
            }
            db::delete_presence(c, &pid)?;
            let (friend_after, own_after) = presence_pair(c, &pid, now, timeout)?;
            let friends = db::friend_ids(c, &pid)?
                .into_iter()
                .map(|(f, _)| f)
                .collect();
            Ok(PresenceChange {
                profile_id: pid,
                friends,
                friend_before,
                friend_after,
                own_before,
                own_after,
            })
        })
        .await?;
    s.push_presence(&change);
    Ok(no_content())
}

pub async fn delete_presence(State(s): State<AppState>, caller: Caller) -> R<Response> {
    go_offline(&s, &caller).await
}

// ---------------------------------------------------------------- invites

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct InviteBody {
    to_profile_id: String,
    join_token: Option<String>,
    message: Option<String>,
}

pub async fn list_invites(State(s): State<AppState>, caller: Caller) -> R<Json<Value>> {
    let now = s.now_ms();
    let v =
        s.db.call(move |c| {
            let incoming = db::invites_for(c, &caller.profile_id, false, now)?
                .iter()
                .map(|i| db::invite_view(c, i))
                .collect::<R<Vec<_>>>()?;
            let outgoing = db::invites_for(c, &caller.profile_id, true, now)?
                .iter()
                .map(|i| db::invite_view(c, i))
                .collect::<R<Vec<_>>>()?;
            Ok(json!({ "incoming": incoming, "outgoing": outgoing }))
        })
        .await?;
    Ok(Json(v))
}

pub async fn send_invite(
    State(s): State<AppState>,
    caller: Caller,
    ApiJson(body): ApiJson<InviteBody>,
) -> R<Response> {
    let to = uuid_param(&body.to_profile_id, "to_profile_id")?;
    if to == caller.profile_id {
        return Err(ApiError::bad_request(
            "cannot_invite_self",
            "you cannot invite yourself",
        ));
    }
    let message = clean_text(
        body.message,
        MAX_INVITE_MESSAGE,
        "message",
        "invalid_invite",
    )?;
    reserved_check(&message, MAX_INVITE_MESSAGE, "message", false)?;
    if body
        .join_token
        .as_ref()
        .is_some_and(|t| t.len() > MAX_JOIN_TOKEN || t.chars().any(char::is_control))
    {
        return Err(ApiError::bad_request(
            "invalid_invite",
            format!("join_token: at most {MAX_JOIN_TOKEN} bytes, no control characters"),
        ));
    }
    let (now, timeout) = (s.now_ms(), s.timeout_ms());
    s.limits.invite.check(&caller.profile_id, now)?;
    let me = caller.profile_id.clone();
    let explicit_token = body.join_token.filter(|t| !t.is_empty());
    let (view, replaced) =
        s.db.tx(move |c| {
            if !db::are_friends(c, &me, &to)? {
                return Err(ApiError::forbidden(
                    "not_friends",
                    "you can only invite friends",
                ));
            }
            let live = db::presence(c, &me)?.filter(|r| now - r.last_heartbeat_ms <= timeout);
            let join_token = explicit_token
                .or_else(|| live.as_ref().and_then(|r| r.join_token.clone()))
                .ok_or_else(|| {
                    ApiError::conflict(
                        "not_joinable",
                        "no join_token: set a joinable presence or pass join_token",
                    )
                })?;
            let replaced: Vec<InviteRow> = db::invites_between_pair(c, &me, &to)?
                .into_iter()
                .filter(|i| i.from_profile == me)
                .collect();
            for i in &replaced {
                db::delete_invite(c, &i.invite_id)?;
            }
            if db::outgoing_invite_count(c, &me, now)? >= OUTGOING_INVITE_LIMIT {
                return Err(ApiError::conflict(
                    "too_many_invites",
                    format!("you have {OUTGOING_INVITE_LIMIT} live invites"),
                ));
            }
            let row = InviteRow {
                invite_id: new_id(),
                from_profile: me.clone(),
                to_profile: to.clone(),
                join_token,
                title_id: live.as_ref().and_then(|r| r.title_id.clone()),
                title_name: live.as_ref().and_then(|r| r.title_name.clone()),
                mode: live.as_ref().and_then(|r| r.mode.clone()),
                message,
                created_ms: now,
                expires_ms: now + INVITE_TTL_MS,
            };
            db::insert_invite(c, &row)?;
            Ok((db::invite_view(c, &row)?, replaced))
        })
        .await?;
    close_invites(&s, &replaced, "replaced");
    s.hub.send(
        &view.to_profile_id,
        &Event::Invite {
            invite: view.clone(),
        },
    );
    Ok((StatusCode::CREATED, Json(json!({ "invite": view }))).into_response())
}

async fn answer_invite(
    s: &AppState,
    caller: &Caller,
    id: String,
    accepted: bool,
) -> R<db::InviteView> {
    let now = s.now_ms();
    let me = caller.profile_id.clone();
    let view =
        s.db.tx(move |c| {
            let i = db::invite(c, &id)?
                .filter(|i| i.to_profile == me)
                .ok_or_else(|| ApiError::not_found("invite_not_found", "no such invite"))?;
            if i.expires_ms <= now {
                return Err(ApiError::gone("invite_expired", "this invite has expired"));
            }
            db::delete_invite(c, &i.invite_id)?;
            db::invite_view(c, &i)
        })
        .await?;
    s.hub.send(
        &view.from.profile_id,
        &Event::InviteResponse {
            invite_id: view.invite_id.clone(),
            profile_id: caller.profile_id.clone(),
            accepted,
        },
    );
    s.hub.send(
        &caller.profile_id,
        &Event::InviteClosed {
            invite_id: view.invite_id.clone(),
            reason: if accepted { "accepted" } else { "declined" }.into(),
        },
    );
    Ok(view)
}

pub async fn accept_invite(
    State(s): State<AppState>,
    caller: Caller,
    ApiPath(id): ApiPath<String>,
) -> R<Json<Value>> {
    let view = answer_invite(&s, &caller, id, true).await?;
    Ok(Json(json!({ "invite": view })))
}

pub async fn decline_invite(
    State(s): State<AppState>,
    caller: Caller,
    ApiPath(id): ApiPath<String>,
) -> R<Response> {
    answer_invite(&s, &caller, id, false).await?;
    Ok(no_content())
}

pub async fn cancel_invite(
    State(s): State<AppState>,
    caller: Caller,
    ApiPath(id): ApiPath<String>,
) -> R<Response> {
    let me = caller.profile_id.clone();
    let i =
        s.db.tx(move |c| {
            let i = db::invite(c, &id)?
                .filter(|i| i.from_profile == me)
                .ok_or_else(|| ApiError::not_found("invite_not_found", "no such invite"))?;
            db::delete_invite(c, &i.invite_id)?;
            Ok(i)
        })
        .await?;
    close_invites(&s, &[i], "cancelled");
    Ok(no_content())
}
