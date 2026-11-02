//! Test harness: the real router over in-memory SQLite, a settable clock, a capturing mailer, the filesystem blob
//! store in a temp dir (or any other store), and a mock for outbound HTTP. No network.
#![allow(dead_code)]

use axum::Router;
use axum::body::Body;
use axum::http::{Request, StatusCode};
use napple_account::app::{AppState, TestClock, router};
use napple_account::blob::{BlobStore, FsBlobStore};
use napple_account::config::Config;
use napple_account::mailer::CaptureMailer;
use napple_account::outbound::{MockOutbound, json_response};
use napple_account::util::{b64url, random_bytes};
use napple_account::{Deps, build_app, db, jwt::JwtKeys};
use serde_json::{Value, json};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;
use tower::ServiceExt;

pub const T0: i64 = 1_793_577_600; // 2026-11-02
pub const ORIGIN: &str = "https://pretend.napple.lat";

pub struct T {
    pub app: AppState,
    pub router: Router,
    pub clock: Arc<TestClock>,
    pub mail: Arc<CaptureMailer>,
    pub out: Arc<MockOutbound>,
    pub dir: PathBuf,
}

pub struct Session {
    pub access: String,
    pub refresh: String,
    pub account_id: String,
    pub device_id: String,
    pub pubkey: Vec<u8>,
    pub email: String,
}

pub fn temp_dir(tag: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!(
        "napple-account-test-{tag}-{}",
        napple_account::util::new_id()
    ));
    std::fs::create_dir_all(&d).unwrap();
    d
}

impl T {
    pub fn new() -> T {
        T::with(&[], None)
    }

    pub fn with(vars: &[(&str, &str)], blobs: Option<Arc<dyn BlobStore>>) -> T {
        let dir = temp_dir("srv");
        let mut v: HashMap<String, String> = HashMap::new();
        v.insert("NAPPLE_ACCOUNT_DATA".into(), dir.display().to_string());
        // Tiny Argon2 memory so flows run fast in debug builds (the production cost is checked in password.rs).
        v.insert("NAPPLE_ACCOUNT_ARGON_M_KIB".into(), "64".into());
        for (k, val) in vars {
            v.insert(k.to_string(), val.to_string());
        }
        let cfg = Config::from_vars(v).unwrap();
        let clock = Arc::new(TestClock::new(T0));
        let mail = Arc::new(CaptureMailer::default());
        let out = Arc::new(MockOutbound::new(|_| json_response(404, json!({}))));
        let (blobs, dev) = match blobs {
            Some(b) => (b, None),
            None => {
                let fs = Arc::new(FsBlobStore::new(dir.join("blobs"), &cfg.public_base).unwrap());
                (fs.clone() as Arc<dyn BlobStore>, Some(fs))
            }
        };
        let app = build_app(
            cfg,
            db::open_memory().unwrap(),
            JwtKeys::generate(),
            Deps {
                clock: clock.clone(),
                mailer: mail.clone(),
                blobs,
                dev_blobs: dev,
                out: out.clone(),
            },
        );
        T {
            router: router(app.clone()),
            app,
            clock,
            mail,
            out,
            dir,
        }
    }

    pub async fn raw(&self, req: Request<Body>) -> (StatusCode, axum::http::HeaderMap, Vec<u8>) {
        let r = self.router.clone().oneshot(req).await.unwrap();
        let status = r.status();
        let headers = r.headers().clone();
        let body = axum::body::to_bytes(r.into_body(), usize::MAX)
            .await
            .unwrap()
            .to_vec();
        (status, headers, body)
    }

    pub async fn req(
        &self,
        method: &str,
        path: &str,
        token: Option<&str>,
        body: Option<Value>,
    ) -> (StatusCode, Value) {
        let mut b = Request::builder()
            .method(method)
            .uri(format!("/account/v1{path}"));
        if let Some(t) = token {
            b = b.header("authorization", format!("Bearer {t}"));
        }
        let req = match body {
            Some(v) => b
                .header("content-type", "application/json")
                .body(Body::from(v.to_string()))
                .unwrap(),
            None => b.body(Body::empty()).unwrap(),
        };
        let (s, _, body) = self.raw(req).await;
        (s, serde_json::from_slice(&body).unwrap_or(Value::Null))
    }

    /// The 6-digit code in the newest mail to `email`.
    pub fn code_for(&self, email: &str) -> String {
        let m = self.mail.last_to(email).expect("a code mail");
        m.text
            .lines()
            .map(str::trim)
            .find(|l| l.len() == 6 && l.bytes().all(|b| b.is_ascii_digit()))
            .expect("a code line")
            .to_string()
    }

    pub fn device_json(name: &str, platform: &str, pubkey: &[u8]) -> Value {
        json!({"name": name, "platform": platform, "pubkey": b64url(pubkey)})
    }

    /// Signs up (new address: signup -> code -> verify) or logs in (known address) with `PASSWORD`.
    pub async fn sign_in_with(
        &self,
        email: &str,
        device: &str,
        platform: &str,
        pubkey: &[u8],
    ) -> Session {
        let dev = T::device_json(device, platform, pubkey);
        let (mut s, mut v) = self
            .req("POST", "/auth/login", None, Some(json!({"email": email, "password": PASSWORD, "device": dev})))
            .await;
        if s == StatusCode::UNAUTHORIZED {
            // Nickname = the email's local part when it is a valid, free one, else a random one.
            let local = email.split('@').next().unwrap_or("").to_string();
            let mut nick = local.clone();
            let (mut s2, mut v2) = (StatusCode::BAD_REQUEST, Value::Null);
            for attempt in 0..2 {
                (s2, v2) = self
                    .req("POST", "/auth/signup", None, Some(json!({"email": email, "password": PASSWORD, "nickname": nick, "age_ok": true, "terms_ok": true, "device": dev})))
                    .await;
                if s2 == StatusCode::ACCEPTED || attempt == 1 {
                    break;
                }
                nick = format!("p{}", hex::encode(random_bytes::<5>()));
            }
            assert_eq!(s2, StatusCode::ACCEPTED, "{v2}");
            let code = self.code_for(&email.to_ascii_lowercase());
            (s, v) = self
                .req("POST", "/auth/verify", None, Some(json!({"email": email, "code": code})))
                .await;
        }
        assert_eq!(s, StatusCode::OK, "{v}");
        Session {
            access: v["access_token"].as_str().unwrap().into(),
            refresh: v["refresh_token"].as_str().unwrap().into(),
            account_id: v["account_id"].as_str().unwrap().into(),
            device_id: v["device_id"].as_str().unwrap().into(),
            pubkey: pubkey.to_vec(),
            email: email.to_ascii_lowercase(),
        }
    }

    pub async fn sign_in(&self, email: &str, device: &str) -> Session {
        self.sign_in_with(email, device, "macos", &random_bytes::<32>())
            .await
    }

    pub async fn first_profile(&self, s: &Session) -> String {
        let (_, v) = self.req("GET", "/profiles", Some(&s.access), None).await;
        v["profiles"][0]["profile_id"].as_str().unwrap().to_string()
    }

    /// Uploads `data` to a dev-blob presigned URL through the router.
    pub async fn dev_put(&self, create: &Value, data: &[u8], content_type: &str) -> StatusCode {
        let url = create["upload_url"]
            .as_str()
            .unwrap()
            .trim_start_matches(ORIGIN)
            .to_string();
        let mut b = Request::builder().method("PUT").uri(url);
        for (k, v) in create["upload_headers"].as_object().unwrap() {
            if k != "content-type" {
                b = b.header(k.as_str(), v.as_str().unwrap());
            }
        }
        let (s, _, _) = self
            .raw(
                b.header("content-type", content_type)
                    .body(Body::from(data.to_vec()))
                    .unwrap(),
            )
            .await;
        s
    }

    pub async fn dev_get(&self, url: &str) -> (StatusCode, Vec<u8>) {
        let (s, _, b) = self
            .raw(
                Request::builder()
                    .uri(url.trim_start_matches(ORIGIN))
                    .body(Body::empty())
                    .unwrap(),
            )
            .await;
        (s, b)
    }

    /// create + upload + commit one version; returns the commit response.
    pub async fn push(
        &self,
        s: &Session,
        title: &str,
        profile: &str,
        base: i64,
        data: &[u8],
    ) -> (StatusCode, Value) {
        let sha = napple_account::util::sha256_hex(data);
        let (st, c) = self
            .req(
                "POST",
                &format!("/saves/{title}/{profile}/versions"),
                Some(&s.access),
                Some(json!({"base_version": base, "size": data.len(), "sha256_ciphertext": sha})),
            )
            .await;
        if st != StatusCode::CREATED {
            return (st, c);
        }
        assert_eq!(
            self.dev_put(&c, data, napple_account::blob::SAVE_CONTENT_TYPE)
                .await,
            StatusCode::OK
        );
        let v = c["version"].as_i64().unwrap();
        self.req(
            "POST",
            &format!("/saves/{title}/{profile}/versions/{v}/commit"),
            Some(&s.access),
            None,
        )
        .await
    }
}

pub const LAS: &str = "01006BB00C6F0000";
pub const SMO: &str = "0100000000010000";
pub const MK8D: &str = "0100152000022000";
pub const DAY: i64 = 86_400;
pub const PASSWORD: &str = "correct horse battery";
