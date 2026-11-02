//! napple-social: the Napple friends and presence service (Napple's own community service; it never talks to Nintendo).
//!
//! REST JSON under `/social/v1` plus one WebSocket at `/social/v1/stream`. Identity comes from the Napple Account
//! service (Ed25519 JWT access tokens + the account's profile list); the social graph is per profile.
//! See docs/guides/napple-social.md for the API and data model.

pub mod api;
pub mod auth;
pub mod clock;
pub mod codes;
pub mod config;
pub mod db;
pub mod error;
pub mod hub;
pub mod ratelimit;
pub mod stream;

use std::net::SocketAddr;
use std::ops::Deref;
use std::sync::Arc;
use std::time::Duration;

use axum::extract::{ConnectInfo, DefaultBodyLimit, FromRequestParts};
use axum::http::request::Parts;
use axum::http::{HeaderValue, header};
use axum::routing::{delete, get, post, put};
use axum::{Json, Router};
use serde_json::json;

use crate::auth::{Auth, ProfileInfo};
use crate::clock::Clock;
use crate::config::Config;
use crate::db::{Db, PresenceView, friend_presence, own_presence};
use crate::error::ApiError;
use crate::hub::{Event, Hub};
use crate::ratelimit::RateLimiter;

pub const PROFILE_HEADER: &str = "x-napple-profile";

pub struct Limits {
    pub ip: RateLimiter,
    pub account: RateLimiter,
    pub friend_request: RateLimiter,
    pub invite: RateLimiter,
    pub code_regen: RateLimiter,
}

pub struct Inner {
    pub config: Config,
    pub clock: Arc<dyn Clock>,
    pub db: Db,
    pub hub: Hub,
    pub auth: Auth,
    pub limits: Limits,
}

#[derive(Clone)]
pub struct AppState(Arc<Inner>);

impl Deref for AppState {
    type Target = Inner;
    fn deref(&self) -> &Inner {
        &self.0
    }
}

impl AppState {
    pub fn new(config: Config, db: Db, clock: Arc<dyn Clock>) -> Self {
        let limits = Limits {
            ip: RateLimiter::per_minute(config.ip_rate_per_min),
            account: RateLimiter::per_minute(config.rate_per_min),
            friend_request: RateLimiter::new(
                config.friend_requests_per_hour.div_ceil(2),
                config.friend_requests_per_hour,
                3_600_000,
            ),
            invite: RateLimiter::new(
                config.invites_per_hour.div_ceil(4),
                config.invites_per_hour,
                3_600_000,
            ),
            code_regen: RateLimiter::new(3, 3, 86_400_000),
        };
        let auth = Auth::new(&config);
        AppState(Arc::new(Inner {
            config,
            clock,
            db,
            hub: Hub::default(),
            auth,
            limits,
        }))
    }

    pub fn now_ms(&self) -> i64 {
        self.clock.now_ms()
    }

    pub fn timeout_ms(&self) -> i64 {
        self.config.presence_timeout_ms()
    }
}

/// An authenticated request: verified token + an owned, registered profile.
#[derive(Debug, Clone)]
pub struct Caller {
    pub account_id: String,
    pub device_id: String,
    pub profile_id: String,
    pub token_exp: i64,
}

fn client_ip(parts: &Parts, trust_proxy: bool) -> String {
    if trust_proxy
        && let Some(v) = parts
            .headers
            .get("x-forwarded-for")
            .and_then(|v| v.to_str().ok())
    {
        // The last hop is the one our own load balancer appended; earlier entries are client-controlled.
        if let Some(last) = v.rsplit(',').map(str::trim).find(|s| !s.is_empty()) {
            return last.to_string();
        }
    }
    parts
        .extensions
        .get::<ConnectInfo<SocketAddr>>()
        .map(|c| c.0.ip().to_string())
        .unwrap_or_else(|| "unknown".into())
}

pub fn bearer(parts: &Parts) -> Result<&str, ApiError> {
    let v = parts.headers.get(header::AUTHORIZATION).ok_or_else(|| {
        ApiError::unauthorized(
            "missing_token",
            "Authorization: Bearer <Napple access token> required",
        )
    })?;
    let v = v
        .to_str()
        .map_err(|_| ApiError::unauthorized("invalid_token", "bad Authorization header"))?;
    let (scheme, token) = v
        .split_once(' ')
        .ok_or_else(|| ApiError::unauthorized("invalid_token", "bad Authorization header"))?;
    if !scheme.eq_ignore_ascii_case("bearer") || token.trim().is_empty() {
        return Err(ApiError::unauthorized(
            "invalid_token",
            "bad Authorization header",
        ));
    }
    Ok(token.trim())
}

impl FromRequestParts<AppState> for Caller {
    type Rejection = ApiError;

    async fn from_request_parts(parts: &mut Parts, state: &AppState) -> Result<Self, ApiError> {
        let now = state.now_ms();
        state
            .limits
            .ip
            .check(&client_ip(parts, state.config.trust_proxy), now)?;
        let token = bearer(parts)?.to_string();
        let claims = state.auth.verify(&token, now).await?;
        state.limits.account.check(&claims.account_id, now)?;
        let raw = parts
            .headers
            .get(PROFILE_HEADER)
            .ok_or_else(|| {
                ApiError::bad_request("missing_profile", "X-Napple-Profile header required")
            })?
            .to_str()
            .map_err(|_| {
                ApiError::bad_request("invalid_profile", "X-Napple-Profile must be a profile UUID")
            })?;
        let profile_id = uuid::Uuid::parse_str(raw.trim())
            .map_err(|_| {
                ApiError::bad_request("invalid_profile", "X-Napple-Profile must be a profile UUID")
            })?
            .hyphenated()
            .to_string();
        let ProfileInfo {
            profile_id,
            nickname,
            icon_id,
        } = state
            .auth
            .owned_profile(&token, &claims.account_id, &profile_id, now)
            .await?;
        let account_id = claims.account_id.clone();
        let pid = profile_id.clone();
        state
            .db
            .call(move |c| {
                db::ensure_profile(c, &account_id, &pid, &nickname, &icon_id, now).map(|_| ())
            })
            .await?;
        Ok(Caller {
            account_id: claims.account_id,
            device_id: claims.device_id,
            profile_id,
            token_exp: claims.exp,
        })
    }
}

/// Before/after snapshot of a profile's presence, for pushing updates.
pub struct PresenceChange {
    pub profile_id: String,
    pub friends: Vec<String>,
    pub friend_before: PresenceView,
    pub friend_after: PresenceView,
    pub own_before: PresenceView,
    pub own_after: PresenceView,
}

/// Reads the friend-visible and own presence of a profile inside a DB call.
pub fn presence_pair(
    c: &rusqlite::Connection,
    profile_id: &str,
    now_ms: i64,
    timeout_ms: i64,
) -> Result<(PresenceView, PresenceView), ApiError> {
    let p = db::must_profile(c, profile_id)?;
    let pr = db::presence(c, profile_id)?;
    Ok((
        friend_presence(&p, pr.as_ref(), now_ms, timeout_ms),
        own_presence(&p, pr.as_ref(), now_ms, timeout_ms),
    ))
}

impl AppState {
    /// Pushes presence updates: to friends when what they see changed, to the profile's own devices when its real state changed.
    pub fn push_presence(&self, ch: &PresenceChange) {
        if !ch.friend_before.same_state(&ch.friend_after) {
            self.hub.send_many(
                &ch.friends,
                &Event::Presence {
                    profile_id: ch.profile_id.clone(),
                    presence: ch.friend_after.clone(),
                },
            );
        }
        if !ch.own_before.same_state(&ch.own_after) {
            self.hub.send(
                &ch.profile_id,
                &Event::Presence {
                    profile_id: ch.profile_id.clone(),
                    presence: ch.own_after.clone(),
                },
            );
        }
    }
}

/// Periodic housekeeping: presence timeouts (offline after 3 missed heartbeats), invite expiry, cache pruning.
/// The server runs it every few seconds; tests call it directly after moving the clock.
pub async fn sweep(state: &AppState) -> Result<(), ApiError> {
    let now = state.now_ms();
    let timeout = state.timeout_ms();
    let changes = state
        .db
        .tx(move |c| {
            let mut out = Vec::new();
            for pid in db::expired_presence(c, now, timeout)? {
                let p = db::must_profile(c, &pid)?;
                let pr = db::presence(c, &pid)?;
                // "Before" as friends saw it right before the timeout.
                let at = pr.as_ref().map_or(now, |r| r.last_heartbeat_ms);
                let friend_before = friend_presence(&p, pr.as_ref(), at, timeout);
                let own_before = own_presence(&p, pr.as_ref(), at, timeout);
                db::delete_presence(c, &pid)?;
                let (friend_after, own_after) = presence_pair(c, &pid, now, timeout)?;
                let friends = db::friend_ids(c, &pid)?
                    .into_iter()
                    .map(|(f, _)| f)
                    .collect();
                out.push(PresenceChange {
                    profile_id: pid,
                    friends,
                    friend_before,
                    friend_after,
                    own_before,
                    own_after,
                });
            }
            let expired = db::expired_invites(c, now)?;
            for i in &expired {
                db::delete_invite(c, &i.invite_id)?;
            }
            Ok((out, expired))
        })
        .await?;
    let (presence, invites) = changes;
    for ch in &presence {
        tracing::debug!(profile = %ch.profile_id, "presence timed out");
        state.push_presence(ch);
    }
    for i in &invites {
        let ev = Event::InviteClosed {
            invite_id: i.invite_id.clone(),
            reason: "expired".into(),
        };
        state.hub.send(&i.to_profile, &ev);
        state.hub.send(&i.from_profile, &ev);
    }
    state.limits.ip.prune(now);
    state.limits.account.prune(now);
    state.limits.friend_request.prune(now);
    state.limits.invite.prune(now);
    state.limits.code_regen.prune(now);
    state.auth.prune(now);
    Ok(())
}

async fn healthz(
    axum::extract::State(state): axum::extract::State<AppState>,
) -> Result<Json<serde_json::Value>, ApiError> {
    state
        .db
        .call(|c| Ok(c.query_row("SELECT 1", [], |r| r.get::<_, i64>(0))?))
        .await?;
    Ok(Json(json!({
        "status": "ok",
        "service": "napple-social",
        "version": env!("CARGO_PKG_VERSION"),
        "connections": state.hub.total(),
        "server_time_ms": state.now_ms(),
    })))
}

pub fn router(state: AppState) -> Router {
    let v1 = Router::new()
        .route("/healthz", get(healthz))
        .route("/me", get(api::me).delete(api::delete_me))
        .route("/me/friend-code/regenerate", post(api::regenerate_code))
        .route("/settings", get(api::get_settings).put(api::put_settings))
        .route("/friends", get(api::friends))
        .route("/friends/{profile_id}", delete(api::remove_friend))
        .route(
            "/friend-requests",
            get(api::list_requests).post(api::send_request),
        )
        .route("/friend-requests/{request_id}", delete(api::cancel_request))
        .route(
            "/friend-requests/{request_id}/accept",
            post(api::accept_request),
        )
        .route(
            "/friend-requests/{request_id}/decline",
            post(api::decline_request),
        )
        .route("/blocks", get(api::list_blocks))
        .route("/blocks/{profile_id}", put(api::block).delete(api::unblock))
        .route(
            "/presence",
            get(api::get_presence)
                .put(api::put_presence)
                .delete(api::delete_presence),
        )
        .route("/presence/heartbeat", post(api::heartbeat))
        .route("/invites", get(api::list_invites).post(api::send_invite))
        .route("/invites/{invite_id}", delete(api::cancel_invite))
        .route("/invites/{invite_id}/accept", post(api::accept_invite))
        .route("/invites/{invite_id}/decline", post(api::decline_invite))
        .route("/stream", get(stream::stream));
    Router::new()
        .nest("/social/v1", v1)
        .fallback(|| async { ApiError::not_found("not_found", "no such endpoint") })
        .layer(DefaultBodyLimit::max(16 * 1024))
        .layer(axum::middleware::from_fn(access_log))
        .with_state(state)
}

/// One log line per request (method, path, status, latency; never headers or bodies) and `Cache-Control: no-store`.
async fn access_log(
    req: axum::extract::Request,
    next: axum::middleware::Next,
) -> axum::response::Response {
    let (method, path) = (req.method().clone(), req.uri().path().to_string());
    let started = std::time::Instant::now();
    let mut resp = next.run(req).await;
    resp.headers_mut()
        .insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
    let status = resp.status().as_u16();
    let ms = started.elapsed().as_millis() as u64;
    if path.ends_with("/healthz") {
        tracing::debug!(%method, %path, status, ms, "request");
    } else if status >= 500 {
        tracing::warn!(%method, %path, status, ms, "request");
    } else {
        tracing::info!(%method, %path, status, ms, "request");
    }
    resp
}

/// Spawns the housekeeping loop.
pub fn spawn_sweeper(state: AppState) -> tokio::task::JoinHandle<()> {
    tokio::spawn(async move {
        let mut tick = tokio::time::interval(Duration::from_secs(5));
        tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
        loop {
            tick.tick().await;
            if let Err(e) = sweep(&state).await {
                tracing::warn!(error = %e, "sweep failed");
            }
        }
    })
}

/// Opens storage under `NAPPLE_SOCIAL_DATA_DIR`, binds and serves until SIGINT/SIGTERM.
pub async fn run(config: Config) -> Result<(), String> {
    std::fs::create_dir_all(&config.data_dir)
        .map_err(|e| format!("create {}: {e}", config.data_dir.display()))?;
    let db = Db::open(&config.db_path())
        .map_err(|e| format!("open {}: {e}", config.db_path().display()))?;
    let state = AppState::new(config.clone(), db, Arc::new(clock::SystemClock));
    let sweeper = spawn_sweeper(state.clone());
    let listener = tokio::net::TcpListener::bind(config.bind)
        .await
        .map_err(|e| format!("bind {}: {e}", config.bind))?;
    tracing::info!(bind = %config.bind, account = %config.account_url, data = %config.data_dir.display(), "napple-social listening");
    axum::serve(
        listener,
        router(state).into_make_service_with_connect_info::<SocketAddr>(),
    )
    .with_graceful_shutdown(shutdown_signal())
    .await
    .map_err(|e| format!("serve: {e}"))?;
    sweeper.abort();
    tracing::info!("napple-social stopped");
    Ok(())
}

async fn shutdown_signal() {
    let ctrl_c = async {
        let _ = tokio::signal::ctrl_c().await;
    };
    #[cfg(unix)]
    let term = async {
        if let Ok(mut s) = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())
        {
            s.recv().await;
        }
    };
    #[cfg(not(unix))]
    let term = std::future::pending::<()>();
    tokio::select! { _ = ctrl_c => {}, _ = term => {} }
}
