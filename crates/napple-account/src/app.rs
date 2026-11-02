//! Shared state, the router, the bearer-token extractor, client IP and rate limits.

use crate::blob::{BlobStore, FsBlobStore};
use crate::config::Config;
use crate::error::{ApiError, ApiResult};
use crate::jwt::JwtKeys;
use crate::mailer::Mailer;
use crate::outbound::Outbound;
use axum::Router;
use axum::extract::{ConnectInfo, DefaultBodyLimit, FromRequestParts};
use axum::http::request::Parts;
use axum::http::{HeaderValue, Method, header};
use axum::routing::{delete, get, post, put};
use rusqlite::{Connection, OptionalExtension, params};
use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};

pub trait Clock: Send + Sync {
    fn now(&self) -> i64;
}

pub struct SystemClock;

impl Clock for SystemClock {
    fn now(&self) -> i64 {
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs() as i64)
            .unwrap_or(0)
    }
}

/// Settable clock for tests (30-day switch rule, 90-day read-only window, token expiry).
pub struct TestClock(pub AtomicI64);

impl TestClock {
    pub fn new(t: i64) -> TestClock {
        TestClock(AtomicI64::new(t))
    }
    pub fn advance(&self, secs: i64) {
        self.0.fetch_add(secs, Ordering::SeqCst);
    }
}

impl Clock for TestClock {
    fn now(&self) -> i64 {
        self.0.load(Ordering::SeqCst)
    }
}

/// Fixed-window counters keyed by `bucket:key` (client IP, email). In memory: one instance behind one proxy.
#[derive(Default)]
pub struct RateLimiter {
    windows: Mutex<HashMap<String, (i64, u32)>>,
}

impl RateLimiter {
    /// Ok, or Err(seconds until the window resets).
    pub fn check(&self, key: &str, limit: u32, window: i64, now: i64) -> Result<(), u64> {
        let mut w = self.windows.lock().unwrap();
        if w.len() > 100_000 {
            w.retain(|_, (start, _)| now - *start < 3600);
        }
        let e = w.entry(key.to_string()).or_insert((now, 0));
        if now - e.0 >= window {
            *e = (now, 0);
        }
        if e.1 >= limit {
            return Err((e.0 + window - now).max(1) as u64);
        }
        e.1 += 1;
        Ok(())
    }
}

pub struct App {
    pub cfg: Config,
    pub db: Mutex<Connection>,
    pub clock: Arc<dyn Clock>,
    pub jwt: JwtKeys,
    pub mailer: Arc<dyn Mailer>,
    pub blobs: Arc<dyn BlobStore>,
    /// Set when the dev filesystem backend is in use (enables `/dev-blobs/`).
    pub dev_blobs: Option<Arc<FsBlobStore>>,
    pub out: Arc<dyn Outbound>,
    pub limiter: RateLimiter,
    /// HMAC key for stored email codes (per process: a restart only voids codes still in flight).
    pub code_key: [u8; 32],
}

pub type AppState = Arc<App>;

impl App {
    pub fn db(&self) -> MutexGuard<'_, Connection> {
        self.db.lock().unwrap_or_else(|e| e.into_inner())
    }
    pub fn now(&self) -> i64 {
        self.clock.now()
    }
}

/// An authenticated request: a valid access token for a live, non-revoked device of an existing account.
#[derive(Clone, Debug)]
pub struct Authed {
    pub account_id: String,
    pub device_id: String,
}

impl FromRequestParts<AppState> for Authed {
    type Rejection = ApiError;

    async fn from_request_parts(parts: &mut Parts, app: &AppState) -> Result<Authed, ApiError> {
        let h = parts
            .headers
            .get(header::AUTHORIZATION)
            .and_then(|v| v.to_str().ok())
            .ok_or_else(|| ApiError::unauthorized("missing bearer token"))?;
        let token = h
            .strip_prefix("Bearer ")
            .or_else(|| h.strip_prefix("bearer "))
            .ok_or_else(|| ApiError::unauthorized("missing bearer token"))?;
        let now = app.now();
        let claims = app
            .jwt
            .verify(token.trim(), &app.cfg.issuer, now)
            .map_err(|_| ApiError::unauthorized("invalid or expired token"))?;
        let db = app.db();
        let row: Option<(i64, Option<i64>, i64)> = db
            .query_row(
                "SELECT d.session_epoch, d.revoked_at, d.last_seen FROM devices d JOIN accounts a ON a.id = d.account_id WHERE d.id = ?1 AND d.account_id = ?2",
                params![claims.did, claims.sub],
                |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
            )
            .optional()?;
        match row {
            Some((epoch, None, last_seen)) if epoch == claims.sep => {
                if now - last_seen > 300 {
                    db.execute(
                        "UPDATE devices SET last_seen = ?1 WHERE id = ?2",
                        params![now, claims.did],
                    )?;
                    db.execute(
                        "UPDATE accounts SET last_seen = ?1 WHERE id = ?2",
                        params![now, claims.sub],
                    )?;
                }
                Ok(Authed {
                    account_id: claims.sub,
                    device_id: claims.did,
                })
            }
            _ => Err(ApiError::unauthorized("session ended")),
        }
    }
}

/// The caller's IP: the last `X-Forwarded-For` hop when behind the trusted proxy (Caddy), else the socket peer.
pub fn client_ip(
    app: &App,
    headers: &axum::http::HeaderMap,
    ext: &axum::http::Extensions,
) -> String {
    if app.cfg.trust_proxy
        && let Some(ip) = headers
            .get("x-forwarded-for")
            .and_then(|v| v.to_str().ok())
            .and_then(|v| v.rsplit(',').next())
    {
        return ip.trim().to_string();
    }
    ext.get::<ConnectInfo<SocketAddr>>()
        .map(|c| c.0.ip().to_string())
        .unwrap_or_else(|| "local".into())
}

/// Extractor for the client IP (for rate limits).
pub struct ClientIp(pub String);

impl FromRequestParts<AppState> for ClientIp {
    type Rejection = ApiError;
    async fn from_request_parts(parts: &mut Parts, app: &AppState) -> Result<ClientIp, ApiError> {
        Ok(ClientIp(client_ip(app, &parts.headers, &parts.extensions)))
    }
}

pub fn limit(app: &App, bucket: &str, key: &str, limit: u32, window: i64) -> ApiResult<()> {
    app.limiter
        .check(&format!("{bucket}:{key}"), limit, window, app.now())
        .map_err(ApiError::too_many)
}

pub fn router(app: AppState) -> Router {
    use crate::{account, admin, keys, pages, passkey, saves, signin};
    let api = Router::new()
        .route("/healthz", get(pages::healthz))
        .route("/.well-known/jwks.json", get(pages::jwks))
        .route("/auth/email/start", post(signin::email_start))
        .route("/auth/email/finish", post(signin::email_finish))
        .route("/auth/email/link", get(pages::email_link))
        .route(
            "/auth/passkey/register/begin",
            post(passkey::register_begin),
        )
        .route(
            "/auth/passkey/register/finish",
            post(passkey::register_finish),
        )
        .route("/auth/passkey/login/begin", post(passkey::login_begin))
        .route("/auth/passkey/login/finish", post(passkey::login_finish))
        .route("/auth/refresh", post(signin::refresh))
        .route("/auth/logout", post(signin::logout))
        .route(
            "/me",
            get(account::me)
                .patch(account::patch_me)
                .delete(account::delete_me),
        )
        .route("/passkeys", get(passkey::list))
        .route("/passkeys/{id}", delete(passkey::remove))
        .route(
            "/profiles",
            get(account::list_profiles).post(account::create_profile),
        )
        .route(
            "/profiles/{id}",
            get(account::get_profile)
                .patch(account::patch_profile)
                .delete(account::delete_profile),
        )
        .route("/devices", get(account::list_devices))
        .route("/devices/{id}", delete(account::revoke_device))
        .route("/keys/requests", get(keys::requests))
        .route("/keys/grant", post(keys::grant))
        .route("/keys/mine", get(keys::mine))
        .route(
            "/keys/recovery",
            get(keys::get_recovery).post(keys::put_recovery),
        )
        .route("/saves", get(saves::list))
        .route(
            "/admin/grants",
            get(admin::list).post(admin::add).delete(admin::remove),
        )
        .route("/saves/{title}", delete(saves::delete_title))
        .route(
            "/saves/{title}/{profile}/versions",
            get(saves::versions).post(saves::create_version),
        )
        .route(
            "/saves/{title}/{profile}/versions/{v}",
            get(saves::get_version),
        )
        .route(
            "/saves/{title}/{profile}/versions/{v}/commit",
            post(saves::commit_version),
        )
        .layer(DefaultBodyLimit::max(64 * 1024));

    let mut api = api;
    if app.dev_blobs.is_some() {
        let max = app.cfg.max_version_bytes as usize + 1;
        api = api.route(
            "/dev-blobs/{*key}",
            put(saves::dev_blob_put)
                .get(saves::dev_blob_get)
                .layer(DefaultBodyLimit::max(max)),
        );
    }

    let origins: Vec<HeaderValue> = app
        .cfg
        .cors_origins
        .iter()
        .filter_map(|o| HeaderValue::from_str(o).ok())
        .collect();
    let cors = tower_http::cors::CorsLayer::new()
        .allow_origin(origins)
        .allow_methods([Method::GET, Method::POST, Method::PATCH, Method::DELETE])
        .allow_headers([header::AUTHORIZATION, header::CONTENT_TYPE])
        .max_age(std::time::Duration::from_secs(600));

    Router::new()
        .route("/healthz", get(pages::healthz))
        .nest("/account/v1", api)
        .layer(cors)
        .with_state(app)
}
