//! Test harness: a mock Napple Account service (JWKS + profile list) on a local port, the social service on another,
//! a manual clock, tokens minted with locally generated Ed25519 keys, and HTTP / WebSocket helpers.
#![allow(dead_code)]

use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use axum::Router;
use axum::extract::State;
use axum::http::{HeaderMap, StatusCode};
use axum::routing::get;
use base64::Engine;
use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use ed25519_dalek::{Signer, SigningKey};
use futures_util::{SinkExt, StreamExt};
use napple_social::AppState;
use napple_social::clock::ManualClock;
use napple_social::config::Config;
use napple_social::db::Db;
use serde_json::{Value, json};
use tokio_tungstenite::tungstenite::Message;
use tokio_tungstenite::tungstenite::client::IntoClientRequest;

pub type Ws =
    tokio_tungstenite::WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>;

#[derive(Default)]
pub struct MockData {
    /// Published keys (kid, key).
    pub keys: Vec<(String, SigningKey)>,
    /// account id -> profile JSON objects.
    pub profiles: HashMap<String, Vec<Value>>,
    pub fail_profiles: bool,
}

#[derive(Clone, Default)]
pub struct Mock {
    pub data: Arc<Mutex<MockData>>,
    pub jwks_hits: Arc<AtomicUsize>,
    pub profile_hits: Arc<AtomicUsize>,
    pub directory_hits: Arc<AtomicUsize>,
}

pub const SOCIAL_SERVICE_TOKEN: &str = "social-service-secret";
pub const ACCOUNT_SERVICE_TOKEN: &str = "account-service-secret";

fn decode_sub(token: &str) -> Option<String> {
    let p = token.split('.').nth(1)?;
    let v: Value = serde_json::from_slice(&URL_SAFE_NO_PAD.decode(p).ok()?).ok()?;
    v.get("sub")?.as_str().map(String::from)
}

async fn jwks(State(m): State<Mock>) -> axum::Json<Value> {
    m.jwks_hits.fetch_add(1, Ordering::SeqCst);
    let d = m.data.lock().unwrap();
    let keys: Vec<Value> = d
        .keys
        .iter()
        .map(|(kid, k)| json!({"kty": "OKP", "crv": "Ed25519", "kid": kid, "use": "sig", "alg": "EdDSA", "x": URL_SAFE_NO_PAD.encode(k.verifying_key().to_bytes())}))
        .collect();
    axum::Json(json!({ "keys": keys }))
}

async fn profiles(
    State(m): State<Mock>,
    headers: HeaderMap,
) -> Result<axum::Json<Value>, StatusCode> {
    m.profile_hits.fetch_add(1, Ordering::SeqCst);
    let auth = headers
        .get("authorization")
        .and_then(|v| v.to_str().ok())
        .ok_or(StatusCode::UNAUTHORIZED)?;
    let token = auth
        .strip_prefix("Bearer ")
        .ok_or(StatusCode::UNAUTHORIZED)?;
    let sub = decode_sub(token).ok_or(StatusCode::UNAUTHORIZED)?;
    let d = m.data.lock().unwrap();
    if d.fail_profiles {
        return Err(StatusCode::INTERNAL_SERVER_ERROR);
    }
    Ok(axum::Json(
        json!({ "profiles": d.profiles.get(&sub).cloned().unwrap_or_default() }),
    ))
}

/// Service-credential profile lookup (what napple-social calls to learn a target's current tier).
async fn directory(
    State(m): State<Mock>,
    axum::extract::Path(id): axum::extract::Path<String>,
    headers: HeaderMap,
) -> Result<axum::Json<Value>, StatusCode> {
    m.directory_hits.fetch_add(1, Ordering::SeqCst);
    if headers
        .get("x-napple-internal")
        .and_then(|v| v.to_str().ok())
        != Some(ACCOUNT_SERVICE_TOKEN)
    {
        return Err(StatusCode::UNAUTHORIZED);
    }
    let d = m.data.lock().unwrap();
    d.profiles
        .values()
        .flatten()
        .find(|p| p["profile_id"] == id.as_str())
        .cloned()
        .map(axum::Json)
        .ok_or(StatusCode::NOT_FOUND)
}

pub fn mint(key: &SigningKey, kid: &str, claims: &Value) -> String {
    let h = URL_SAFE_NO_PAD
        .encode(serde_json::to_vec(&json!({"alg": "EdDSA", "typ": "JWT", "kid": kid})).unwrap());
    let p = URL_SAFE_NO_PAD.encode(serde_json::to_vec(claims).unwrap());
    let sig = key.sign(format!("{h}.{p}").as_bytes());
    format!("{h}.{p}.{}", URL_SAFE_NO_PAD.encode(sig.to_bytes()))
}

pub fn uuid7() -> String {
    uuid::Uuid::now_v7().hyphenated().to_string()
}

/// One player profile on one account, with a valid token.
#[derive(Clone, Debug)]
pub struct User {
    pub account_id: String,
    pub device_id: String,
    pub profile_id: String,
    pub nickname: String,
    pub token: String,
}

pub struct Env {
    pub base: String,
    pub ws_base: String,
    pub state: AppState,
    pub clock: Arc<ManualClock>,
    pub mock: Mock,
    pub key: SigningKey,
    pub kid: String,
    pub http: reqwest::Client,
    _dir: tempfile::TempDir,
}

pub fn test_config(account_url: &str, dir: &std::path::Path) -> Config {
    let mut c = Config::from_lookup(|_| None).unwrap();
    c.account_url = account_url.to_string();
    c.data_dir = dir.to_path_buf();
    c.rate_per_min = 100_000;
    c.ip_rate_per_min = 100_000;
    c.friend_requests_per_hour = 10_000;
    c.invites_per_hour = 10_000;
    c.service_token = Some(SOCIAL_SERVICE_TOKEN.into());
    c.account_service_token = Some(ACCOUNT_SERVICE_TOKEN.into());
    c.profile_ttl_secs = 0;
    c
}

impl Env {
    pub async fn start() -> Env {
        Self::start_with(|_| {}).await
    }

    pub async fn start_with(tweak: impl FnOnce(&mut Config)) -> Env {
        let mock = Mock::default();
        let key = SigningKey::from_bytes(&rand::random::<[u8; 32]>());
        let kid = "test-key-1".to_string();
        mock.data
            .lock()
            .unwrap()
            .keys
            .push((kid.clone(), key.clone()));
        let mock_app = Router::new()
            .route("/account/v1/.well-known/jwks.json", get(jwks))
            .route("/account/v1/profiles", get(profiles))
            .route("/account/v1/internal/profiles/{id}", get(directory))
            .with_state(mock.clone());
        let ml = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let account_url = format!("http://{}", ml.local_addr().unwrap());
        tokio::spawn(async move { axum::serve(ml, mock_app).await.unwrap() });

        let dir = tempfile::tempdir().unwrap();
        let mut config = test_config(&account_url, dir.path());
        tweak(&mut config);
        let db = Db::open(&config.db_path()).unwrap();
        let clock = Arc::new(ManualClock::starting_now());
        let state = AppState::new(config, db, clock.clone());
        let app = napple_social::router(state.clone());
        let l = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = l.local_addr().unwrap();
        tokio::spawn(async move {
            axum::serve(l, app.into_make_service_with_connect_info::<SocketAddr>())
                .await
                .unwrap()
        });
        Env {
            base: format!("http://{addr}/social/v1"),
            ws_base: format!("ws://{addr}/social/v1"),
            state,
            clock,
            mock,
            key,
            kid,
            http: reqwest::Client::builder().no_proxy().build().unwrap(),
            _dir: dir,
        }
    }

    pub fn now_s(&self) -> i64 {
        use napple_social::clock::Clock;
        self.clock.now_ms() / 1000
    }

    pub fn claims(&self, account_id: &str, device_id: &str) -> Value {
        json!({"sub": account_id, "did": device_id, "aud": "napple", "iat": self.now_s(), "exp": self.now_s() + 900, "iss": "napple-account"})
    }

    pub fn token_for(&self, account_id: &str, device_id: &str) -> String {
        mint(&self.key, &self.kid, &self.claims(account_id, device_id))
    }

    /// A new account with `n` profiles registered at the mock account service.
    pub fn account(&self, nicknames: &[&str]) -> Vec<User> {
        let account_id = uuid7();
        let device_id = format!("dev-{}", &uuid7()[..8]);
        let token = self.token_for(&account_id, &device_id);
        let users: Vec<User> = nicknames
            .iter()
            .map(|n| User {
                account_id: account_id.clone(),
                device_id: device_id.clone(),
                profile_id: uuid7(),
                nickname: n.to_string(),
                token: token.clone(),
            })
            .collect();
        let profiles = users
            .iter()
            .enumerate()
            .map(|(i, u)| json!({"profile_id": u.profile_id, "nickname": u.nickname, "icon_id": i}))
            .collect();
        self.mock
            .data
            .lock()
            .unwrap()
            .profiles
            .insert(account_id, profiles);
        users
    }

    pub fn user(&self, nickname: &str) -> User {
        self.account(&[nickname]).remove(0)
    }

    pub async fn call_raw(
        &self,
        method: &str,
        path: &str,
        token: Option<&str>,
        profile: Option<&str>,
        body: Option<Value>,
    ) -> (StatusCode, Value) {
        let url = format!("{}{}", self.base, path);
        let m = reqwest::Method::from_bytes(method.as_bytes()).unwrap();
        let mut rb = self.http.request(m, url);
        if let Some(t) = token {
            rb = rb.bearer_auth(t);
        }
        if let Some(p) = profile {
            rb = rb.header("X-Napple-Profile", p);
        }
        if let Some(b) = body {
            rb = rb.json(&b);
        }
        let resp = rb.send().await.unwrap();
        let status = StatusCode::from_u16(resp.status().as_u16()).unwrap();
        let text = resp.text().await.unwrap();
        let v = if text.is_empty() {
            Value::Null
        } else {
            serde_json::from_str(&text).unwrap_or(Value::String(text))
        };
        (status, v)
    }

    pub async fn call(
        &self,
        u: &User,
        method: &str,
        path: &str,
        body: Option<Value>,
    ) -> (StatusCode, Value) {
        self.call_raw(method, path, Some(&u.token), Some(&u.profile_id), body)
            .await
    }

    pub async fn get(&self, u: &User, path: &str) -> Value {
        let (s, v) = self.call(u, "GET", path, None).await;
        assert_eq!(s, StatusCode::OK, "GET {path}: {v}");
        v
    }

    pub async fn code_of(&self, u: &User) -> String {
        self.get(u, "/me").await["friend_code"]
            .as_str()
            .unwrap()
            .to_string()
    }

    /// Makes two profiles friends through the public API.
    pub async fn befriend(&self, a: &User, b: &User) {
        let code = self.code_of(b).await;
        let (s, v) = self
            .call(
                a,
                "POST",
                "/friend-requests",
                Some(json!({"friend_code": code})),
            )
            .await;
        assert_eq!(s, StatusCode::CREATED, "{v}");
        let id = v["request"]["request_id"].as_str().unwrap().to_string();
        let (s, v) = self
            .call(b, "POST", &format!("/friend-requests/{id}/accept"), None)
            .await;
        assert_eq!(s, StatusCode::OK, "{v}");
    }

    pub async fn ws(&self, u: &User) -> Ws {
        let mut req = format!("{}/stream", self.ws_base)
            .into_client_request()
            .unwrap();
        req.headers_mut().insert(
            "authorization",
            format!("Bearer {}", u.token).parse().unwrap(),
        );
        req.headers_mut()
            .insert("x-napple-profile", u.profile_id.parse().unwrap());
        let (mut ws, _) = tokio_tungstenite::connect_async(req)
            .await
            .expect("ws connect");
        let hello = next_event(&mut ws, "hello").await;
        assert_eq!(hello["profile_id"], u.profile_id.as_str());
        ws
    }

    /// Changes a profile's tier at the mock account service (what `PATCH /profiles/{id}` would do).
    pub fn set_tier(&self, u: &User, tier: i64) {
        let mut d = self.mock.data.lock().unwrap();
        for p in d.profiles.get_mut(&u.account_id).unwrap() {
            if p["profile_id"] == u.profile_id.as_str() {
                p["tier"] = json!(tier);
            }
        }
    }

    /// A user whose account lists the given tier.
    pub fn user_tier(&self, nickname: &str, tier: i64) -> User {
        let u = self.user(nickname);
        self.set_tier(&u, tier);
        u
    }

    /// POST with the napple-online service credential.
    pub async fn service(
        &self,
        method: &str,
        path: &str,
        body: Option<Value>,
    ) -> (StatusCode, Value) {
        let m = reqwest::Method::from_bytes(method.as_bytes()).unwrap();
        let mut rb = self
            .http
            .request(m, format!("{}{}", self.base, path))
            .header("X-Napple-Service", SOCIAL_SERVICE_TOKEN);
        if let Some(b) = body {
            rb = rb.json(&b);
        }
        let resp = rb.send().await.unwrap();
        let status = StatusCode::from_u16(resp.status().as_u16()).unwrap();
        let text = resp.text().await.unwrap();
        let v = if text.is_empty() {
            Value::Null
        } else {
            serde_json::from_str(&text).unwrap_or(Value::String(text))
        };
        (status, v)
    }

    pub async fn sweep(&self) {
        napple_social::sweep(&self.state).await.unwrap();
    }
}

/// Next event of the given type (skipping others), within 5 s.
pub async fn next_event(ws: &mut Ws, ty: &str) -> Value {
    let deadline = tokio::time::Instant::now() + Duration::from_secs(5);
    loop {
        let msg = tokio::time::timeout_at(deadline, ws.next())
            .await
            .unwrap_or_else(|_| panic!("timed out waiting for {ty}"));
        match msg {
            Some(Ok(Message::Text(t))) => {
                let v: Value = serde_json::from_str(&t).unwrap();
                if v["type"] == ty {
                    return v;
                }
            }
            Some(Ok(Message::Close(f))) => panic!("closed while waiting for {ty}: {f:?}"),
            Some(Ok(_)) => {}
            other => panic!("stream ended waiting for {ty}: {other:?}"),
        }
    }
}

/// Asserts no event of the given type arrives within `ms`.
pub async fn no_event(ws: &mut Ws, ty: &str, ms: u64) {
    let deadline = tokio::time::Instant::now() + Duration::from_millis(ms);
    loop {
        match tokio::time::timeout_at(deadline, ws.next()).await {
            Err(_) => return,
            Ok(Some(Ok(Message::Text(t)))) => {
                let v: Value = serde_json::from_str(&t).unwrap();
                assert_ne!(v["type"], ty, "unexpected {ty}: {v}");
            }
            Ok(Some(Ok(_))) => {}
            Ok(_) => return,
        }
    }
}

pub async fn ws_send(ws: &mut Ws, v: Value) {
    ws.send(Message::Text(v.to_string().into())).await.unwrap();
}

/// Next event of the given type whose `field` equals `value` (skipping others), within 5 s.
pub async fn next_event_where(ws: &mut Ws, ty: &str, field: &str, value: &str) -> Value {
    loop {
        let v = next_event(ws, ty).await;
        if v[field] == value {
            return v;
        }
    }
}
