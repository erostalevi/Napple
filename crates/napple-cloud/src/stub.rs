//! A local, in-memory implementation of the account API (orchestrations/ACCOUNT-CONTRACT.md) for tests and offline development.
//! Not the server (crates/napple-account is): no passkeys, no email delivery, no persistence, opaque random tokens instead of JWTs.
//!
//! - Email sign-in: the code is `fixed_code` when set, else random and printed to stdout.
//! - Presigned URLs: `<base>/_blob/<id>` on the stub itself (PUT/GET, no Authorization header, like S3 presigned URLs).
//! - Plans: Free = 1 synced title (the first title that gets a version), 10 versions; Plus = unlimited, 20 versions.
//!   `POST <base>/_stub/plan {email, plan}` switches an account's plan; 402 {reason:"plan_limit", upgrade_url} beyond the limit.
//! - Versions: linear per (title, profile); a new version is head + 1 and needs `base_version == head` (else 409 {error, head}).
use crate::util::{hex, now, random, sha256_hex};
use serde_json::{json, Value};
use std::collections::{BTreeMap, HashMap};
use std::io::Read;
use std::sync::{Arc, Mutex};

#[derive(Clone, Default)]
pub struct Options {
    pub fixed_code: Option<String>,
    /// Access token lifetime in seconds (default 900).
    pub access_ttl: Option<u64>,
    /// "free" (default) or "plus" for new accounts.
    pub default_plan: Option<String>,
    pub quiet: bool,
}

#[derive(Default)]
struct Account {
    id: String,
    email: String,
    display_name: String,
    plan: String,
    devices: Vec<Device>,
    profiles: Vec<Value>,
    recovery: Option<Value>,
    free_title: Option<String>,
}

#[derive(Clone)]
struct Device {
    id: String,
    name: String,
    platform: String,
    pubkey: String,
    created_at: u64,
    wrapped: Option<String>,
    revoked: bool,
}

#[derive(Clone)]
struct Version {
    version: u64,
    size: u64,
    sha: String,
    device: String,
    created_at: u64,
    committed: bool,
    blob: Option<String>,
}

#[derive(Default)]
struct State {
    base: String,
    opts: Options,
    codes: HashMap<String, String>,
    accounts: Vec<Account>,
    access: HashMap<String, (usize, String, u64)>,
    refresh: HashMap<String, (usize, String)>,
    saves: BTreeMap<(usize, String, String), Vec<Version>>,
    blobs: HashMap<String, Vec<u8>>,
    /// Requests seen, for tests ("METHOD /path status").
    log: Vec<String>,
}

pub struct Stub {
    pub base: String,
    server: Arc<tiny_http::Server>,
    state: Arc<Mutex<State>>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl Drop for Stub {
    fn drop(&mut self) {
        self.server.unblock();
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

type Reply = (u16, Value);

fn err(code: u16, msg: &str) -> Reply {
    (code, json!({"error": msg}))
}

fn token() -> String {
    hex(&random::<24>())
}

impl Stub {
    /// Serves on `addr` ("127.0.0.1:0" for a free port); `base` is the API base URL (".../account/v1").
    pub fn start(addr: &str, opts: Options) -> std::io::Result<Stub> {
        let server = Arc::new(tiny_http::Server::http(addr).map_err(|e| std::io::Error::other(e.to_string()))?);
        let port = server.server_addr().to_ip().map(|a| a.port()).unwrap_or(0);
        let host = addr.rsplit_once(':').map(|(h, _)| h).unwrap_or("127.0.0.1");
        let host = if host == "0.0.0.0" { "127.0.0.1" } else { host };
        let base = format!("http://{host}:{port}/account/v1");
        let state = Arc::new(Mutex::new(State { base: base.clone(), opts, ..Default::default() }));
        let (srv, st) = (server.clone(), state.clone());
        let thread = std::thread::spawn(move || {
            for mut rq in srv.incoming_requests() {
                let mut body = Vec::new();
                let _ = rq.as_reader().take(80 << 20).read_to_end(&mut body);
                let method = rq.method().as_str().to_string();
                let url = rq.url().to_string();
                let auth = rq
                    .headers()
                    .iter()
                    .find(|h| h.field.equiv("Authorization"))
                    .map(|h| h.value.as_str().trim_start_matches("Bearer ").to_string());
                let mut g = st.lock().unwrap();
                let (code, resp_body, ctype) = match g.route_raw(&method, &url, &body) {
                    Some((c, b)) => (c, b, "application/octet-stream"),
                    None => {
                        let (c, v) = g.route(&method, &url, auth.as_deref(), &body);
                        (c, if v.is_null() { Vec::new() } else { v.to_string().into_bytes() }, "application/json")
                    }
                };
                g.log.push(format!("{method} {url} {code}"));
                if !g.opts.quiet {
                    println!("[stub] {method} {url} -> {code}");
                }
                drop(g);
                let h = tiny_http::Header::from_bytes(&b"Content-Type"[..], ctype.as_bytes()).unwrap();
                let _ = rq.respond(tiny_http::Response::from_data(resp_body).with_status_code(code).with_header(h));
            }
        });
        Ok(Stub { base, server, state, thread: Some(thread) })
    }

    pub fn log(&self) -> Vec<String> {
        self.state.lock().unwrap().log.clone()
    }

    pub fn set_plan(&self, email: &str, plan: &str) {
        let mut g = self.state.lock().unwrap();
        if let Some(a) = g.accounts.iter_mut().find(|a| a.email == email) {
            a.plan = plan.into();
        }
    }

    /// Expires every access token (the next authenticated call gets 401 and the client refreshes).
    pub fn expire_access_tokens(&self) {
        self.state.lock().unwrap().access.clear();
    }

    /// The stored ciphertext of every committed version (tests check that the server never sees plaintext).
    pub fn all_blobs(&self) -> Vec<Vec<u8>> {
        self.state.lock().unwrap().blobs.values().cloned().collect()
    }

    pub fn join(mut self) {
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

impl State {
    fn tokens(&mut self, acc: usize, dev: &str) -> Value {
        let (a, r) = (token(), token());
        let ttl = self.opts.access_ttl.unwrap_or(900);
        self.access.insert(a.clone(), (acc, dev.to_string(), now() + ttl));
        self.refresh.insert(r.clone(), (acc, dev.to_string()));
        json!({"access_token": a, "refresh_token": r, "expires_in": ttl, "token_type": "Bearer",
               "account_id": self.accounts[acc].id, "device_id": dev})
    }

    fn plan_json(&self, a: &Account) -> Value {
        let plus = a.plan == "plus";
        json!({"name": if plus {"plus"} else {"free"}, "status": "active", "renews_at": null,
               "synced_titles_limit": if plus { Value::Null } else { json!(1) },
               "quota": if plus { 5u64 << 30 } else { 256u64 << 20 },
               "upgrade_url": format!("{}/billing/checkout", self.base),
               "synced_titles": a.free_title.iter().collect::<Vec<_>>()})
    }

    fn route_raw(&mut self, method: &str, url: &str, body: &[u8]) -> Option<(u16, Vec<u8>)> {
        let id = url.strip_prefix("/account/v1/_blob/")?.to_string();
        Some(match method {
            "PUT" => {
                if body.len() > 64 << 20 {
                    return Some((413, Vec::new()));
                }
                self.blobs.insert(id, body.to_vec());
                (200, Vec::new())
            }
            "GET" => match self.blobs.get(&id) {
                Some(b) => (200, b.clone()),
                None => (404, Vec::new()),
            },
            _ => (405, Vec::new()),
        })
    }

    fn route(&mut self, method: &str, url: &str, auth: Option<&str>, body: &[u8]) -> Reply {
        let b: Value = serde_json::from_slice(body).unwrap_or(Value::Null);
        let Some(path) = url.split('?').next().and_then(|u| u.strip_prefix("/account/v1")) else { return err(404, "not found") };
        let seg: Vec<&str> = path.trim_matches('/').split('/').collect();
        match (method, seg.as_slice()) {
            ("GET", ["healthz"]) => return (200, json!({"ok": true})),
            ("POST", ["auth", "email", "start"]) => {
                let Some(email) = b["email"].as_str().filter(|e| e.contains('@')) else { return err(400, "email") };
                let code = self.opts.fixed_code.clone().unwrap_or_else(|| format!("{:06}", u32::from_le_bytes(random::<4>()) % 1_000_000));
                if !self.opts.quiet || self.opts.fixed_code.is_none() {
                    println!("[stub] sign-in code for {email}: {code}");
                }
                self.codes.insert(email.to_lowercase(), code);
                return (200, json!({"sent": true}));
            }
            ("POST", ["auth", "email", "finish"]) => {
                let email = b["email"].as_str().unwrap_or("").to_lowercase();
                if self.codes.get(&email).map(String::as_str) != b["code"].as_str() {
                    return err(401, "bad code");
                }
                self.codes.remove(&email);
                let d = &b["device"];
                let Some(pk) = d["pubkey"].as_str().filter(|p| crate::util::unb64(p).is_some_and(|k| k.len() == 32)) else {
                    return err(400, "device.pubkey");
                };
                let acc = match self.accounts.iter().position(|a| a.email == email) {
                    Some(i) => i,
                    None => {
                        let plan = self.opts.default_plan.clone().unwrap_or_else(|| "free".into());
                        self.accounts.push(Account {
                            id: format!("acct-{}", hex(&random::<8>())),
                            display_name: email.split('@').next().unwrap_or("").to_string(),
                            email: email.clone(),
                            plan,
                            ..Default::default()
                        });
                        self.accounts.len() - 1
                    }
                };
                let dev = Device {
                    id: format!("dev-{}", hex(&random::<8>())),
                    name: d["name"].as_str().unwrap_or("device").into(),
                    platform: d["platform"].as_str().unwrap_or("cli").into(),
                    pubkey: pk.into(),
                    created_at: now(),
                    wrapped: None,
                    revoked: false,
                };
                let id = dev.id.clone();
                self.accounts[acc].devices.push(dev);
                return (200, self.tokens(acc, &id));
            }
            ("POST", ["auth", "refresh"]) => {
                let Some((acc, dev)) = b["refresh_token"].as_str().and_then(|r| self.refresh.remove(r)) else { return err(401, "bad refresh token") };
                if self.accounts[acc].devices.iter().any(|d| d.id == dev && d.revoked) {
                    return err(401, "device revoked");
                }
                return (200, self.tokens(acc, &dev));
            }
            ("POST", ["_stub", "plan"]) => {
                let email = b["email"].as_str().unwrap_or("").to_lowercase();
                let Some(a) = self.accounts.iter_mut().find(|a| a.email == email) else { return err(404, "no such account") };
                a.plan = b["plan"].as_str().unwrap_or("free").into();
                return (200, json!({"plan": a.plan}));
            }
            ("GET", ["billing", "checkout"]) => return (200, json!({"page": "stub checkout"})),
            _ => {}
        }
        // authenticated
        let Some((acc, dev)) = auth.and_then(|t| self.access.get(t)).filter(|(_, _, exp)| *exp > now()).map(|(a, d, _)| (*a, d.clone())) else {
            return err(401, "unauthorized");
        };
        if self.accounts[acc].devices.iter().any(|d| d.id == dev && d.revoked) {
            return err(401, "device revoked");
        }
        match (method, seg.as_slice()) {
            ("POST", ["auth", "logout"]) => {
                self.access.retain(|_, v| v.1 != dev);
                self.refresh.retain(|_, v| v.1 != dev);
                (200, json!({}))
            }
            ("GET", ["me"]) => {
                let a = &self.accounts[acc];
                (200, json!({"account_id": a.id, "email": a.email, "display_name": a.display_name, "plan": self.plan_json(a)}))
            }
            ("GET", ["profiles"]) => (200, json!(self.accounts[acc].profiles)),
            ("POST", ["profiles"]) => {
                let a = &mut self.accounts[acc];
                if a.profiles.len() >= 8 {
                    return err(400, "at most 8 profiles");
                }
                let p = json!({"profile_id": format!("prof-{}", hex(&random::<6>())), "nickname": b["nickname"].as_str().unwrap_or("Player"),
                               "icon_id": b["icon_id"].as_u64().unwrap_or(0)});
                a.profiles.push(p.clone());
                (200, p)
            }
            ("GET", ["devices"]) => {
                let v: Vec<Value> = self.accounts[acc]
                    .devices
                    .iter()
                    .filter(|d| !d.revoked)
                    .map(|d| json!({"device_id": d.id, "name": d.name, "platform": d.platform, "pubkey": d.pubkey,
                                    "created_at": d.created_at, "last_seen": now(), "has_key": d.wrapped.is_some()}))
                    .collect();
                (200, json!(v))
            }
            ("DELETE", ["devices", id]) => {
                let Some(d) = self.accounts[acc].devices.iter_mut().find(|d| d.id == *id) else { return err(404, "no such device") };
                d.revoked = true;
                d.wrapped = None;
                (200, json!({}))
            }
            ("GET", ["keys", "requests"]) => {
                let v: Vec<Value> = self.accounts[acc]
                    .devices
                    .iter()
                    .filter(|d| !d.revoked && d.wrapped.is_none())
                    .map(|d| json!({"device_id": d.id, "name": d.name, "platform": d.platform, "pubkey": d.pubkey, "created_at": d.created_at}))
                    .collect();
                (200, json!(v))
            }
            ("POST", ["keys", "grant"]) => {
                let (Some(id), Some(w)) = (b["device_id"].as_str(), b["wrapped_account_key"].as_str()) else { return err(400, "fields") };
                let Some(d) = self.accounts[acc].devices.iter_mut().find(|d| d.id == id && !d.revoked) else { return err(404, "no such device") };
                d.wrapped = Some(w.to_string());
                (200, json!({}))
            }
            ("GET", ["keys", "mine"]) => match self.accounts[acc].devices.iter().find(|d| d.id == dev).and_then(|d| d.wrapped.clone()) {
                Some(w) => (200, json!({"wrapped_account_key": w})),
                None => err(404, "no key for this device yet"),
            },
            ("POST", ["keys", "recovery"]) => {
                self.accounts[acc].recovery = Some(b);
                (200, json!({}))
            }
            ("GET", ["keys", "recovery"]) => match &self.accounts[acc].recovery {
                Some(r) => (200, r.clone()),
                None => err(404, "no recovery record"),
            },
            ("GET", ["saves"]) => {
                let v: Vec<Value> = self
                    .saves
                    .iter()
                    .filter(|((a, _, _), _)| *a == acc)
                    .filter_map(|((_, t, p), vs)| {
                        let h = vs.iter().filter(|v| v.committed).max_by_key(|v| v.version)?;
                        Some(json!({"title_id": t, "profile_id": p, "latest_version": h.version, "size": h.size,
                                    "updated_at": h.created_at, "device_id": h.device}))
                    })
                    .collect();
                (200, json!(v))
            }
            (m, ["saves", title, profile, rest @ ..]) => self.saves_route(acc, &dev, m, title, profile, rest, &b),
            _ => err(404, "not found"),
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn saves_route(&mut self, acc: usize, dev: &str, m: &str, title: &str, profile: &str, rest: &[&str], b: &Value) -> Reply {
        if !self.accounts[acc].profiles.iter().any(|p| p["profile_id"] == profile) {
            return err(404, "no such profile");
        }
        let key = (acc, title.to_string(), profile.to_string());
        let head = self.saves.get(&key).and_then(|vs| vs.iter().filter(|v| v.committed).map(|v| v.version).max()).unwrap_or(0);
        match (m, rest) {
            ("GET", ["versions"]) => {
                let vs: Vec<Value> = self
                    .saves
                    .get(&key)
                    .map(|vs| {
                        vs.iter()
                            .filter(|v| v.committed)
                            .map(|v| json!({"version": v.version, "size": v.size, "sha256_ciphertext": v.sha, "created_at": v.created_at, "device_id": v.device}))
                            .collect()
                    })
                    .unwrap_or_default();
                (200, json!(vs))
            }
            ("POST", ["versions"]) => {
                let base = b["base_version"].as_u64().unwrap_or(u64::MAX);
                if base != head {
                    return (409, json!({"error": "conflict", "head": head}));
                }
                let size = b["size"].as_u64().unwrap_or(0);
                if size > 64 << 20 {
                    return err(413, "a version is at most 64 MiB");
                }
                let a = &self.accounts[acc];
                if a.plan != "plus" {
                    match &a.free_title {
                        Some(t) if t != title => {
                            return (402, json!({"reason": "plan_limit", "upgrade_url": format!("{}/billing/checkout", self.base)}));
                        }
                        _ => {}
                    }
                }
                let id = hex(&random::<16>());
                let v = Version {
                    version: head + 1,
                    size,
                    sha: b["sha256_ciphertext"].as_str().unwrap_or("").to_lowercase(),
                    device: dev.to_string(),
                    created_at: now(),
                    committed: false,
                    blob: Some(id.clone()),
                };
                let vs = self.saves.entry(key).or_default();
                vs.retain(|x| x.committed);
                vs.push(v);
                (200, json!({"version": head + 1, "upload_url": format!("{}/_blob/{id}", self.base)}))
            }
            ("POST", ["versions", n, "commit"]) => {
                let n: u64 = n.parse().unwrap_or(0);
                let free = self.accounts[acc].plan != "plus";
                let Some(v) = self.saves.get_mut(&key).and_then(|vs| vs.iter_mut().find(|v| v.version == n && !v.committed)) else {
                    return err(404, "no such pending version");
                };
                let Some(data) = v.blob.as_ref().and_then(|id| self.blobs.get(id)) else { return err(400, "nothing uploaded") };
                if sha256_hex(data) != v.sha || data.len() as u64 != v.size {
                    return err(400, "uploaded data does not match size/sha256_ciphertext");
                }
                v.committed = true;
                let keep = if free { 10 } else { 20 };
                let vs = self.saves.get_mut(&key).unwrap();
                while vs.iter().filter(|v| v.committed).count() > keep {
                    let i = vs.iter().position(|v| v.committed).unwrap();
                    let old = vs.remove(i);
                    if let Some(id) = old.blob {
                        self.blobs.remove(&id);
                    }
                }
                if free && self.accounts[acc].free_title.is_none() {
                    self.accounts[acc].free_title = Some(title.to_string());
                }
                (200, json!({"version": n}))
            }
            ("GET", ["versions", n]) => {
                let n: u64 = n.parse().unwrap_or(0);
                match self.saves.get(&key).and_then(|vs| vs.iter().find(|v| v.version == n && v.committed)).and_then(|v| v.blob.clone()) {
                    Some(id) => (200, json!({"url": format!("{}/_blob/{id}", self.base)})),
                    None => err(404, "no such version"),
                }
            }
            _ => err(404, "not found"),
        }
    }
}
