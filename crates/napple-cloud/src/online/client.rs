//! The Napple Online account / profile / social client. Blocking; the runtime calls it from a worker thread.
//!
//! Actions take and return JSON (`act`), the UI state is one normalized JSON document (`state`), so the C glue only converts. Result codes
//! are the overlay header's `napple_online_result`. Tokens: the refresh token lives in the shared credentials file (see `creds`); access
//! tokens only in memory. A 401 refreshes once and retries; refreshing is serialized across processes by the credentials lock.
use super::creds::{CredStore, Credentials};
use crate::keys;
use crate::util::{b64, now, random, unb64};
use crate::{Error, Result};
use serde_json::{json, Value};
use std::path::PathBuf;
use std::sync::Mutex;
use std::time::Duration;

pub const OK: i32 = 0;
pub const ERR_NETWORK: i32 = 1;
pub const ERR_REJECTED: i32 = 2;
pub const ERR_UNVERIFIED: i32 = 3;
pub const ERR_RATE_LIMITED: i32 = 4;
pub const ERR_INVALID: i32 = 5;
pub const ERR_INTERNAL: i32 = 6;

/// An HTTP answer; code 0 = the server could not be reached.
pub struct Reply {
    pub code: u16,
    pub body: Value,
}

impl Reply {
    fn ok(&self) -> bool {
        (200..300).contains(&self.code)
    }
    /// The overlay result code and the JSON handed to the completion.
    fn result(&self) -> (i32, Value) {
        if self.ok() {
            return (OK, if self.body.is_null() { json!({}) } else { self.body.clone() });
        }
        let reason = self.body["reason"]
            .as_str()
            .or_else(|| self.body["error"].as_str())
            .or_else(|| self.body["error"]["code"].as_str())
            .map(str::to_string)
            .unwrap_or_else(|| if self.code == 0 { "network".into() } else { format!("http_{}", self.code) });
        let mut out = if self.body.is_object() { self.body.clone() } else { json!({}) };
        out["reason"] = json!(reason);
        let code = match self.code {
            0 => ERR_NETWORK,
            429 => ERR_RATE_LIMITED,
            403 if reason == "unverified" => ERR_UNVERIFIED,
            400..=499 => ERR_REJECTED,
            _ => ERR_INTERNAL,
        };
        (code, out)
    }
}

enum Body<'a> {
    None,
    Json(&'a Value),
    Bytes(&'a str, &'a [u8]),
}

struct St {
    access: Option<(String, u64)>,
    pending_device: Option<([u8; 32], [u8; 32])>,
    state: Value,
    reachable: bool,
}

pub struct Client {
    pub creds: CredStore,
    account: String,
    social: String,
    agent: ureq::Agent,
    st: Mutex<St>,
    local_user: Mutex<String>,
}

fn base(url: &str, suffix: &str) -> String {
    let u = url.trim_end_matches('/');
    let u = u.strip_suffix("/account/v1").or_else(|| u.strip_suffix("/social/v1")).unwrap_or(u);
    format!("{u}{suffix}")
}

fn s<'a>(v: &'a Value, names: &[&str]) -> &'a str {
    names.iter().find_map(|n| v[*n].as_str()).unwrap_or("")
}

fn jwt_claim(token: &str, claim: &str) -> Option<String> {
    let payload = unb64(token.split('.').nth(1)?)?;
    let v: Value = serde_json::from_slice(&payload).ok()?;
    v[claim].as_str().map(str::to_string)
}

fn list<'a>(v: &'a Value, key: &str) -> &'a [Value] {
    v.as_array().or_else(|| v[key].as_array()).map(|a| a.as_slice()).unwrap_or(&[])
}

impl Client {
    pub fn new(home: PathBuf, account_url: &str, social_url: &str) -> Client {
        let agent = ureq::AgentBuilder::new()
            .timeout_connect(Duration::from_secs(5))
            .timeout(Duration::from_secs(20))
            .user_agent(concat!("napple-online/", env!("CARGO_PKG_VERSION")))
            .build();
        Client {
            creds: CredStore::open(&home),
            account: base(account_url, "/account/v1"),
            social: base(social_url, "/social/v1"),
            agent,
            st: Mutex::new(St { access: None, pending_device: None, state: json!({"signed_in": false}), reachable: true }),
            local_user: Mutex::new(String::new()),
        }
    }

    /// Defaults from the environment: NAPPLE_ACCOUNT_URL, NAPPLE_SOCIAL_URL, NAPPLE_ONLINE_SERVER, then the community server.
    pub fn from_env(home: PathBuf) -> Client {
        let online = std::env::var("NAPPLE_ONLINE_SERVER").ok().filter(|v| !v.is_empty() && v != "default").unwrap_or_else(|| "https://pretend.napple.lat".into());
        let acc = std::env::var("NAPPLE_ACCOUNT_URL").ok().filter(|v| !v.is_empty()).unwrap_or_else(|| online.clone());
        let soc = std::env::var("NAPPLE_SOCIAL_URL").ok().filter(|v| !v.is_empty()).unwrap_or(online);
        Client::new(home, &acc, &soc)
    }

    pub fn set_local_user(&self, uid_hex: &str) {
        *self.local_user.lock().unwrap() = uid_hex.to_string();
    }

    fn http(&self, method: &str, url: &str, token: Option<&str>, profile: Option<&str>, body: Body) -> Reply {
        let mut r = self.agent.request(method, url).set("Accept", "application/json");
        if let Some(t) = token {
            r = r.set("Authorization", &format!("Bearer {t}"));
        }
        if let Some(p) = profile.filter(|p| !p.is_empty()) {
            r = r.set("X-Napple-Profile", p);
        }
        let res = match body {
            Body::None => r.call(),
            Body::Json(v) => r.send_json(v.clone()),
            Body::Bytes(ct, b) => r.set("Content-Type", ct).send_bytes(b),
        };
        let (code, text) = match res {
            Ok(resp) => (resp.status(), resp.into_string().unwrap_or_default()),
            Err(ureq::Error::Status(c, resp)) => (c, resp.into_string().unwrap_or_default()),
            Err(_) => {
                self.st.lock().unwrap().reachable = false;
                return Reply { code: 0, body: Value::Null };
            }
        };
        self.st.lock().unwrap().reachable = true;
        let body = if text.trim().is_empty() { Value::Null } else { serde_json::from_str(&text).unwrap_or(Value::Null) };
        Reply { code, body }
    }

    // ---- tokens ----

    pub fn signed_in(&self) -> bool {
        matches!(self.creds.load(), Ok(Some(c)) if !c.refresh_token_enc.is_empty())
    }

    /// A valid access token (refreshing when it is about to expire), or None when signed out.
    pub fn access_token(&self) -> Option<String> {
        if !self.signed_in() {
            return None;
        }
        if let Some((t, exp)) = &self.st.lock().unwrap().access {
            if *exp > now() + 30 {
                return Some(t.clone());
            }
        }
        self.refresh().ok()
    }

    /// Exchanges the stored refresh token (rotating it) under the credentials lock, so two games never present the same token twice.
    pub fn refresh(&self) -> Result<String> {
        let url = format!("{}/auth/refresh", self.account);
        let (access, exp) = self.creds.update(|c| {
            let cur = c.as_mut().ok_or(Error::NotSignedIn)?;
            let rt = String::from_utf8_lossy(&self.creds.open_sealed(&cur.refresh_token_enc)?).into_owned();
            let r = self.http("POST", &url, None, None, Body::Json(&json!({"refresh_token": rt})));
            match r.code {
                0 => Err(Error::Net("account server unreachable".into())),
                200..=299 => {
                    let t = if r.body["tokens"].is_object() { &r.body["tokens"] } else { &r.body };
                    let a = s(t, &["access_token"]).to_string();
                    if a.is_empty() {
                        return Err(Error::Format("refresh answer has no access_token".into()));
                    }
                    if let Some(n) = t["refresh_token"].as_str() {
                        cur.refresh_token_enc = self.creds.seal(n.as_bytes())?;
                    }
                    Ok((a, now() + t["expires_in"].as_u64().unwrap_or(900)))
                }
                401 => {
                    *c = None; // revoked or reused: signed out everywhere on this Mac
                    Err(Error::Unauthorized)
                }
                st => Err(Error::Http { status: st, body: String::new() }),
            }
        })?;
        self.st.lock().unwrap().access = Some((access.clone(), exp));
        Ok(access)
    }

    fn authed(&self, method: &str, url: &str, profile: Option<&str>, body: Body) -> Reply {
        let Some(t) = self.access_token() else { return Reply { code: 401, body: json!({"reason": "signed_out"}) } };
        let r = self.http(method, url, Some(&t), profile, clone_body(&body));
        if r.code != 401 {
            return r;
        }
        match self.refresh() {
            Ok(t) => self.http(method, url, Some(&t), profile, body),
            Err(Error::Net(_)) => Reply { code: 0, body: Value::Null },
            Err(_) => r,
        }
    }

    fn acc(&self, method: &str, path: &str, body: Option<&Value>) -> Reply {
        self.authed(method, &format!("{}{}", self.account, path), None, body.map_or(Body::None, Body::Json))
    }

    fn soc(&self, method: &str, path: &str, body: Option<&Value>) -> Reply {
        let p = self.profile_id();
        self.authed(method, &format!("{}{}", self.social, path), p.as_deref(), body.map_or(Body::None, Body::Json))
    }

    // ---- profile mapping ----

    fn map_path(&self) -> PathBuf {
        self.creds.dir().join("profile_map.json")
    }

    fn read_map(&self) -> Value {
        std::fs::read(self.map_path()).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_else(|| json!({}))
    }

    fn write_map(&self, v: &Value) {
        use std::os::unix::fs::OpenOptionsExt;
        let _ = std::fs::create_dir_all(self.creds.dir());
        let tmp = self.creds.dir().join(format!(".profile_map.{}.tmp", std::process::id()));
        if let Ok(mut f) = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&tmp) {
            use std::io::Write;
            if f.write_all(v.to_string().as_bytes()).is_ok() {
                let _ = std::fs::rename(&tmp, self.map_path());
            }
        }
    }

    /// The Napple profile the local Switch user plays as: its mapping, else the map's default, else the account's first profile (recorded
    /// as the mapping, so the choice sticks without a linking step). `profiles` is the account's list when known.
    pub fn profile_for_user(&self, uid_hex: &str, profiles: &[Value]) -> Option<String> {
        let map = self.read_map();
        let valid = |id: &str| profiles.is_empty() || profiles.iter().any(|p| s(p, &["profile_id", "id"]) == id);
        if let Some(id) = map["users"][uid_hex].as_str().filter(|i| valid(i)) {
            return Some(id.to_string());
        }
        if let Some(id) = map["default"].as_str().filter(|i| valid(i)) {
            return Some(id.to_string());
        }
        let first = profiles.first().map(|p| s(p, &["profile_id", "id"]).to_string()).filter(|i| !i.is_empty())?;
        let mut m = map;
        m["default"] = json!(first);
        self.write_map(&m);
        Some(first)
    }

    fn set_profile(&self, id: &str) {
        let uid = self.local_user.lock().unwrap().clone();
        let mut m = self.read_map();
        if uid.is_empty() {
            m["default"] = json!(id);
        } else {
            m["users"][&uid] = json!(id);
        }
        self.write_map(&m);
    }

    /// The acting profile id from the last state (or the mapping alone before the first refresh).
    pub fn profile_id(&self) -> Option<String> {
        let st = self.st.lock().unwrap();
        let cur = s(&st.state["current"], &["id"]);
        if !cur.is_empty() {
            return Some(cur.to_string());
        }
        drop(st);
        let uid = self.local_user.lock().unwrap().clone();
        self.profile_for_user(&uid, &[])
    }

    // ---- sign-in ----

    fn device_keys(&self) -> ([u8; 32], [u8; 32]) {
        if let Ok(Some(c)) = self.creds.load() {
            if let Ok(sk) = self.creds.open_sealed(&c.device_key_enc) {
                if let Ok(pk) = keys::device_public(&sk) {
                    return (sk.try_into().unwrap_or([0; 32]), pk);
                }
            }
        }
        let mut st = self.st.lock().unwrap();
        *st.pending_device.get_or_insert_with(keys::device_keypair)
    }

    fn device_json(&self) -> Value {
        let (_, pk) = self.device_keys();
        let name = std::env::var("NAPPLE_DEVICE_NAME").unwrap_or_else(|_| crate::session::default_device_name());
        json!({"name": name, "platform": crate::session::platform(), "pubkey": b64(&pk)})
    }

    fn complete_signin(&self, v: &Value, email: &str) -> std::result::Result<(), (i32, Value)> {
        let t = if v["tokens"].is_object() { &v["tokens"] } else { v };
        let (access, refresh) = (s(t, &["access_token"]), s(t, &["refresh_token"]));
        if access.is_empty() || refresh.is_empty() {
            return Err((ERR_INTERNAL, json!({"reason": "sign-in answer has no tokens"})));
        }
        let account_id = Some(s(v, &["account_id"])).filter(|x| !x.is_empty()).map(str::to_string).or_else(|| jwt_claim(access, "sub")).unwrap_or_default();
        let device_id = Some(s(v, &["device_id"])).filter(|x| !x.is_empty()).map(str::to_string).or_else(|| jwt_claim(access, "did")).unwrap_or_default();
        let (sk, _) = self.device_keys();
        let res = self.creds.update(|c| {
            *c = Some(Credentials {
                server: self.account.clone(),
                account_id,
                device_id,
                refresh_token_enc: self.creds.seal(refresh.as_bytes())?,
                device_key_enc: self.creds.seal(&sk)?,
                account_key_enc: String::new(),
                email: email.to_string(),
            });
            Ok(())
        });
        if let Err(e) = res {
            return Err((ERR_INTERNAL, json!({"reason": e.to_string()})));
        }
        self.st.lock().unwrap().access = Some((access.to_string(), now() + t["expires_in"].as_u64().unwrap_or(900)));
        self.st.lock().unwrap().pending_device = None;
        self.setup_keys(s(v, &["key_state"]));
        Ok(())
    }

    fn account_key(&self) -> Option<[u8; 32]> {
        let c = self.creds.load().ok()??;
        if c.account_key_enc.is_empty() {
            return None;
        }
        self.creds.open_sealed(&c.account_key_enc).ok()?.try_into().ok()
    }

    fn store_account_key(&self, k: &[u8; 32]) {
        let _ = self.creds.update(|c| {
            if let Some(c) = c.as_mut() {
                c.account_key_enc = self.creds.seal(k)?;
            }
            Ok(())
        });
    }

    /// The end-to-end save key for this device: made by the first device, fetched when another device already granted it, else pending.
    fn setup_keys(&self, key_state: &str) {
        let Ok(Some(c)) = self.creds.load() else { return };
        if self.account_key().is_some() {
            return;
        }
        let Some(sk) = self.creds.open_sealed(&c.device_key_enc).ok() else { return };
        let fetch = |this: &Self| {
            let r = this.acc("GET", "/keys/mine", None);
            if let Some(w) = r.ok().then(|| unb64(s(&r.body, &["wrapped_account_key"]))).flatten() {
                if let Ok(k) = keys::unwrap_account_key(&sk, &w) {
                    this.store_account_key(&k);
                }
            }
        };
        match key_state {
            "bootstrap" => {
                let k: [u8; 32] = random();
                if let Ok(pk) = keys::device_public(&sk) {
                    if let Ok(w) = keys::wrap_account_key(&k, &pk) {
                        let r = self.acc("POST", "/keys/grant", Some(&json!({"device_id": c.device_id, "wrapped_account_key": b64(&w)})));
                        if r.ok() {
                            self.store_account_key(&k);
                        }
                    }
                }
            }
            "pending" => {}
            _ => fetch(self),
        }
    }

    // ---- state ----

    pub fn state(&self) -> Value {
        self.st.lock().unwrap().state.clone()
    }

    fn photo_path(&self, id: &str, has: bool) -> String {
        if id.is_empty() || !has {
            return String::new();
        }
        let p = self.creds.dir().join("photos").join(format!("{id}.jpg"));
        if p.exists() {
            return p.to_string_lossy().into_owned();
        }
        let r = self.fetch_bytes(&format!("{}/profiles/{id}/photo", self.account));
        match r {
            Some(b) if !b.is_empty() => {
                let _ = std::fs::create_dir_all(p.parent().unwrap());
                if std::fs::write(&p, b).is_ok() {
                    return p.to_string_lossy().into_owned();
                }
                String::new()
            }
            _ => String::new(),
        }
    }

    fn fetch_bytes(&self, url: &str) -> Option<Vec<u8>> {
        use std::io::Read;
        let t = self.access_token()?;
        let resp = self.agent.get(url).set("Authorization", &format!("Bearer {t}")).call().ok()?;
        let mut out = Vec::new();
        resp.into_reader().take(6 << 20).read_to_end(&mut out).ok()?;
        Some(out)
    }

    fn norm_profile(&self, v: &Value, fetch_photo: bool) -> Value {
        let id = s(v, &["profile_id", "id"]);
        let has_photo = !v["photo"].is_null() && v["photo"] != json!(false) && v["photo"] != json!("") || v["has_photo"] == json!(true);
        let links: Vec<Value> = v["links"].as_array().map(|a| a.iter().map(|l| json!({"kind": s(l, &["kind"]), "value": s(l, &["value"])})).collect()).unwrap_or_default();
        json!({
            "id": id, "nickname": s(v, &["nickname", "name"]), "bio": s(v, &["bio"]), "links": links,
            "icon_id": v["icon_id"].as_i64().or_else(|| v["icon"].as_i64()).or_else(|| s(v, &["icon_id", "icon"]).parse().ok()).unwrap_or(0),
            "photo_path": if fetch_photo { self.photo_path(id, has_photo) } else { String::new() },
            "tier": v["tier"].as_i64().unwrap_or(1), "friend_code": s(v, &["friend_code"]),
        })
    }

    fn norm_other(&self, v: &Value) -> Value {
        let p = if v["profile"].is_object() { &v["profile"] } else { v };
        let n = self.norm_profile(p, true);
        json!({"profile_id": n["id"], "nickname": n["nickname"], "photo_path": n["photo_path"], "icon_id": n["icon_id"]})
    }

    /// Pulls everything the overlay shows and replaces the cached state. Returns true when it changed.
    pub fn refresh_state(&self) -> bool {
        let new = self.build_state();
        let mut st = self.st.lock().unwrap();
        let changed = st.state != new;
        st.state = new;
        changed
    }

    fn build_state(&self) -> Value {
        let reachable = self.st.lock().unwrap().reachable;
        let Some(c) = self.creds.load().ok().flatten().filter(|c| !c.refresh_token_enc.is_empty()) else {
            return json!({"signed_in": false, "server_reachable": reachable});
        };
        if self.account_key().is_none() {
            self.setup_keys("granted");
        }
        let me = self.acc("GET", "/me", None);
        if me.code == 401 && !self.signed_in() {
            return json!({"signed_in": false, "server_reachable": true});
        }
        let prof_r = self.acc("GET", "/profiles", None);
        let raw: Vec<Value> = list(&prof_r.body, "profiles").to_vec();
        let uid = self.local_user.lock().unwrap().clone();
        let cur_id = self.profile_for_user(&uid, &raw).unwrap_or_default();
        let mut cur = json!({});
        let profiles: Vec<Value> = raw
            .iter()
            .map(|p| {
                let is_cur = s(p, &["profile_id", "id"]) == cur_id;
                let n = self.norm_profile(p, is_cur);
                if is_cur {
                    cur = n.clone();
                }
                n
            })
            .collect();
        // keep the id known to profile_id() while the rest loads
        self.st.lock().unwrap().state["current"] = json!({"id": cur_id});
        let mut devices: Vec<Value> = list(&self.acc("GET", "/devices", None).body, "devices")
            .iter()
            .map(|d| {
                let id = s(d, &["device_id", "id"]);
                json!({"id": id, "name": s(d, &["name"]), "platform": s(d, &["platform"]), "current": id == c.device_id, "pending": false,
                       "last_seen_unix": d["last_seen"].as_i64().or_else(|| d["last_seen_at"].as_i64()).unwrap_or(0)})
            })
            .collect();
        if self.account_key().is_some() {
            for r in list(&self.acc("GET", "/keys/requests", None).body, "requests") {
                let id = s(r, &["device_id", "id"]);
                if devices.iter().any(|d| d["id"] == id && d["pending"] == false) {
                    for d in devices.iter_mut().filter(|d| d["id"] == id) {
                        d["pending"] = json!(true);
                    }
                } else {
                    devices.push(json!({"id": id, "name": s(r, &["name", "device_name"]), "platform": s(r, &["platform"]), "current": false, "pending": true, "last_seen_unix": 0}));
                }
            }
        }
        let (mut friends, mut requests, mut blocks, mut recent, mut stats) = (vec![], vec![], vec![], vec![], vec![]);
        if !cur_id.is_empty() {
            if cur["friend_code"] == "" {
                let m = self.soc("GET", "/me", None);
                if m.ok() {
                    cur["friend_code"] = json!(s(&m.body, &["friend_code"]));
                }
            }
            for f in list(&self.soc("GET", "/friends", None).body, "friends") {
                let p = &f["presence"];
                let st = s(p, &["status"]);
                let o = self.norm_other(f);
                let mut e = o.clone();
                e["online"] = json!(st == "online" || st == "playing");
                e["title_id"] = json!(s(p, &["title_id"]));
                e["mode"] = json!(s(p, &["mode"]));
                e["joinable_token"] = json!(if p["joinable"] == true { s(p, &["join_token"]) } else { "" });
                friends.push(e);
            }
            let fr = self.soc("GET", "/friend-requests", None).body;
            for (key, incoming) in [("incoming", true), ("outgoing", false)] {
                for r in list(&fr[key], key) {
                    let other = if incoming { &r["from"] } else { &r["to"] };
                    let mut e = self.norm_other(other);
                    e["incoming"] = json!(incoming);
                    e["request_id"] = json!(s(r, &["request_id", "id"]));
                    requests.push(e);
                }
            }
            for b in list(&self.soc("GET", "/blocks", None).body, "blocks") {
                let mut e = self.norm_other(b);
                e["incoming"] = json!(false);
                blocks.push(e);
            }
            for r in list(&self.soc("GET", "/recent", None).body, "recent") {
                let mut e = self.norm_other(r);
                e["incoming"] = json!(false);
                recent.push(e);
            }
            let sr = self.soc("GET", &format!("/profiles/{cur_id}/stats"), None).body;
            for t in list(&sr, "stats") {
                stats.push(json!({"title_id": s(t, &["title_id"]), "title_name": s(t, &["title_name"]), "key": s(t, &["key"]), "value": t["value"].as_i64().unwrap_or(0)}));
            }
            if let Some(m) = sr["titles"].as_object() {
                for (tid, kv) in m {
                    for (k, v) in kv.as_object().into_iter().flatten() {
                        stats.push(json!({"title_id": tid, "title_name": "", "key": k, "value": v.as_i64().unwrap_or(0)}));
                    }
                }
            }
        }
        let reachable = self.st.lock().unwrap().reachable;
        json!({
            "signed_in": true, "email": if s(&me.body, &["email"]).is_empty() { c.email.clone() } else { s(&me.body, &["email"]).to_string() },
            "account_id": c.account_id, "email_verified": me.body["email_verified"].as_bool().unwrap_or(true),
            "server_reachable": reachable, "current": cur, "profiles": profiles, "devices": devices,
            "friends": friends, "requests": requests, "blocks": blocks, "recent": recent, "stats": stats,
            "has_account_key": self.account_key().is_some(),
        })
    }

    // ---- actions ----

    fn find_request(&self, profile_id: &str, incoming: bool) -> Option<String> {
        let st = self.st.lock().unwrap();
        st.state["requests"].as_array()?.iter().find(|r| r["profile_id"] == profile_id && r["incoming"] == incoming).map(|r| s(r, &["request_id"]).to_string())
    }

    fn done(&self, r: Reply) -> (i32, Value) {
        r.result()
    }

    /// Runs one UI action. `args` are the action's named arguments; the answer is (napple_online_result, json).
    pub fn act(&self, action: &str, args: &Value) -> (i32, Value) {
        let a = |k: &str| args[k].as_str().unwrap_or("");
        let invalid = |m: &str| (ERR_INVALID, json!({"reason": m}));
        match action {
            "signup" => {
                if a("email").is_empty() || a("password").is_empty() {
                    return invalid("email and password are required");
                }
                let body = json!({"email": a("email"), "password": a("password"), "nickname": a("nickname"),
                    "age_ok": args["age_ok"].as_bool().unwrap_or(false), "terms_ok": args["terms_ok"].as_bool().unwrap_or(false), "device": self.device_json()});
                if !(body["age_ok"].as_bool().unwrap() && body["terms_ok"].as_bool().unwrap()) {
                    return invalid("age and terms must be accepted");
                }
                self.http("POST", &format!("{}/auth/signup", self.account), None, None, Body::Json(&body)).result()
            }
            "verify" | "login" => {
                let (path, body) = if action == "verify" {
                    ("/auth/verify", json!({"email": a("email"), "code": a("code"), "device": self.device_json()}))
                } else {
                    ("/auth/login", json!({"email": a("email"), "password": a("password"), "device": self.device_json()}))
                };
                let r = self.http("POST", &format!("{}{}", self.account, path), None, None, Body::Json(&body));
                if !r.ok() {
                    return r.result();
                }
                if let Err(e) = self.complete_signin(&r.body, a("email")) {
                    return e;
                }
                self.refresh_state();
                (OK, json!({"signed_in": true}))
            }
            "logout" => {
                if let Ok(Some(rt)) = self.creds.refresh_token() {
                    let _ = self.http("POST", &format!("{}/auth/logout", self.account), None, None, Body::Json(&json!({"refresh_token": rt})));
                }
                let _ = self.creds.update(|c| {
                    *c = None;
                    Ok(())
                });
                let mut st = self.st.lock().unwrap();
                st.access = None;
                st.state = json!({"signed_in": false});
                (OK, json!({}))
            }
            "reset_start" => self.http("POST", &format!("{}/auth/password/reset/start", self.account), None, None, Body::Json(&json!({"email": a("email")}))).result(),
            "reset_finish" => self
                .http("POST", &format!("{}/auth/password/reset/finish", self.account), None, None,
                    Body::Json(&json!({"email": a("email"), "code": a("code"), "new_password": a("new_password")})))
                .result(),
            "switch_profile" => {
                if a("profile_id").is_empty() {
                    return invalid("profile_id");
                }
                self.set_profile(a("profile_id"));
                self.st.lock().unwrap().state["current"] = json!({"id": a("profile_id")});
                self.refresh_state();
                (OK, json!({}))
            }
            "create_profile" => {
                let r = self.acc("POST", "/profiles", Some(&json!({"nickname": a("nickname"), "icon_id": 0})));
                self.done(r)
            }
            "update_profile" => {
                let mut b = args["profile"].clone();
                if !b.is_object() {
                    b = serde_json::from_str(a("profile_json")).unwrap_or(Value::Null);
                }
                if !b.is_object() || a("profile_id").is_empty() {
                    return invalid("profile json");
                }
                self.acc("PATCH", &format!("/profiles/{}", a("profile_id")), Some(&b)).result()
            }
            "upload_photo" => {
                let Ok(bytes) = std::fs::read(a("path")) else { return invalid("cannot read the photo file") };
                if bytes.len() > 5 << 20 {
                    return invalid("photo larger than 5 MiB");
                }
                let bd = format!("napple{}", crate::util::hex(&random::<8>()));
                let mut body = format!("--{bd}\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"photo\"\r\nContent-Type: application/octet-stream\r\n\r\n").into_bytes();
                body.extend(&bytes);
                body.extend(format!("\r\n--{bd}--\r\n").as_bytes());
                let ct = format!("multipart/form-data; boundary={bd}");
                let r = self.authed("POST", &format!("{}/profiles/{}/photo", self.account, a("profile_id")), None, Body::Bytes(&ct, &body));
                if r.ok() {
                    let _ = std::fs::remove_file(self.creds.dir().join("photos").join(format!("{}.jpg", a("profile_id"))));
                }
                r.result()
            }
            "set_tier" => {
                let t = args["tier"].as_i64().unwrap_or(0);
                if !(1..=5).contains(&t) {
                    return invalid("tier 1..5");
                }
                self.acc("PATCH", &format!("/profiles/{}", a("profile_id")), Some(&json!({"tier": t}))).result()
            }
            "friend_request" => {
                let q = a("code");
                let b = if q.len() > 20 || q.contains('-') && q.len() < 8 { json!({"profile_id": q}) } else { json!({"friend_code": q}) };
                self.soc("POST", "/friend-requests", Some(&b)).result()
            }
            "friend_accept" | "friend_decline" => {
                let Some(id) = self.find_request(a("profile_id"), true) else { return invalid("no such request") };
                self.soc("POST", &format!("/friend-requests/{id}/{}", &action[7..]), None).result()
            }
            "friend_cancel" => {
                let Some(id) = self.find_request(a("profile_id"), false) else { return invalid("no such request") };
                self.soc("DELETE", &format!("/friend-requests/{id}"), None).result()
            }
            "friend_remove" => self.soc("DELETE", &format!("/friends/{}", a("profile_id")), None).result(),
            "block" => self.soc("PUT", &format!("/blocks/{}", a("profile_id")), None).result(),
            "unblock" => self.soc("DELETE", &format!("/blocks/{}", a("profile_id")), None).result(),
            "search" => {
                let q: String = a("query").bytes().map(|b| if b.is_ascii_alphanumeric() || b"-_.".contains(&b) { (b as char).to_string() } else { format!("%{b:02X}") }).collect();
                let r = self.soc("GET", &format!("/search?q={q}"), None);
                if !r.ok() {
                    return r.result();
                }
                let results: Vec<Value> = list(&r.body, "results").iter().map(|p| self.norm_other(p)).collect();
                (OK, json!({"results": results}))
            }
            "invite" => self.soc("POST", "/invites", Some(&json!({"to_profile_id": a("profile_id")}))).result(),
            "join" => {
                // accept the friend's live invite when there is one; otherwise hand back the token from their presence
                let inv = self.soc("GET", "/invites", None);
                let id = list(&inv.body["incoming"], "incoming").iter().find(|i| i["from"]["profile_id"] == a("profile_id")).map(|i| s(i, &["invite_id", "id"]).to_string());
                if let Some(id) = id {
                    return self.soc("POST", &format!("/invites/{id}/accept"), None).result();
                }
                let tok = self.state()["friends"].as_array().and_then(|f| f.iter().find(|f| f["profile_id"] == a("profile_id")).map(|f| s(f, &["joinable_token"]).to_string())).unwrap_or_default();
                let tok = if tok.is_empty() { a("token").to_string() } else { tok };
                if tok.is_empty() { invalid("the friend is not joinable") } else { (OK, json!({"join_token": tok})) }
            }
            "report" => self.soc("POST", "/reports", Some(&json!({"profile_id": a("profile_id"), "reason": a("reason")}))).result(),
            "approve_device" => {
                let Some(key) = self.account_key() else { return invalid("this device has no save key to hand over") };
                let reqs = self.acc("GET", "/keys/requests", None);
                let Some(r) = list(&reqs.body, "requests").iter().find(|r| s(r, &["device_id", "id"]) == a("device_id")) else { return invalid("no such pending device") };
                let Some(pk) = unb64(s(r, &["pubkey"])) else { return invalid("device has no public key") };
                let Ok(w) = keys::wrap_account_key(&key, &pk) else { return invalid("bad device public key") };
                self.acc("POST", "/keys/grant", Some(&json!({"device_id": a("device_id"), "wrapped_account_key": b64(&w)}))).result()
            }
            "revoke_device" => self.acc("DELETE", &format!("/devices/{}", a("device_id")), None).result(),
            "delete_account" => {
                let r = self.acc("DELETE", "/me", Some(&json!({"password": a("password")})));
                if r.ok() {
                    return self.act("logout", &json!({}));
                }
                r.result()
            }
            _ => invalid("unknown action"),
        }
    }
}

fn clone_body<'a>(b: &Body<'a>) -> Body<'a> {
    match b {
        Body::None => Body::None,
        Body::Json(v) => Body::Json(v),
        Body::Bytes(c, d) => Body::Bytes(c, d),
    }
}
