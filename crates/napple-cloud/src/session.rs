//! A signed-in device: tokens and their refresh, the account save key, sign-in/out, device approval, recovery, profiles.
use crate::api::{list, str_field, Api};
use crate::keys;
use crate::state::{AccountConfig, PlanInfo, StateDir};
use crate::store::{self, SecretStore};
use crate::util::{b64, now, unb64};
use crate::{Error, Result};
use serde_json::{json, Value};
use std::path::Path;

pub struct Session {
    pub dir: StateDir,
    pub store: Box<dyn SecretStore>,
    pub api: Api,
    cfg: std::sync::Mutex<AccountConfig>,
}

/// macos | ios | cli (contract `device.platform`). The CLI, Studio and built games on a Mac share one device identity, so a Mac is "macos".
pub fn platform() -> &'static str {
    if cfg!(target_os = "ios") {
        "ios"
    } else if cfg!(target_os = "macos") {
        "macos"
    } else {
        "cli"
    }
}

pub fn default_device_name() -> String {
    let mut buf = [0u8; 256];
    let ok = unsafe { libc::gethostname(buf.as_mut_ptr() as *mut libc::c_char, buf.len()) } == 0;
    let host = if ok { String::from_utf8_lossy(&buf[..buf.iter().position(|&b| b == 0).unwrap_or(0)]).to_string() } else { String::new() };
    let host = host.trim_end_matches(".local").to_string();
    if host.is_empty() { format!("Napple ({})", platform()) } else { host }
}

/// The server URL: `NAPPLE_ACCOUNT_URL`, else the production service.
pub fn default_server() -> String {
    std::env::var("NAPPLE_ACCOUNT_URL").ok().filter(|s| !s.is_empty()).unwrap_or_else(|| crate::state::DEFAULT_SERVER.to_string())
}

/// Step 1 of the email sign-in: the server mails a link and a 6-digit code.
pub fn email_start(server: &str, email: &str) -> Result<()> {
    Api::new(server).call("POST", "/auth/email/start", None, Some(&json!({ "email": email })))?;
    Ok(())
}

pub enum KeySetup {
    /// This device can decrypt the account's saves.
    Ready,
    /// First device of the account: a new account save key was made; show the recovery code once.
    CreatedFirst { recovery_phrase: String },
    /// Another device has to approve this one (or the recovery code has to be entered).
    WaitingApproval,
}

impl Session {
    pub fn open(support: &Path) -> Result<Session> {
        Self::open_ns(support, &crate::state::namespace())
    }

    pub fn open_ns(support: &Path, ns: &str) -> Result<Session> {
        let dir = StateDir::new(support, ns);
        let cfg = dir.load_account()?.ok_or(Error::NotSignedIn)?;
        let store = store::open(ns, &dir.root)?;
        if store.get(store::REFRESH_TOKEN)?.is_none() {
            return Err(Error::NotSignedIn);
        }
        let api = Api::new(&cfg.server);
        Ok(Session { dir, store, api, cfg: std::sync::Mutex::new(cfg) })
    }

    /// Step 2 of the email sign-in: registers this device (new X25519 key) and stores the tokens.
    pub fn email_finish(support: &Path, ns: &str, server: &str, email: &str, code: &str, device_name: &str) -> Result<Session> {
        let dir = StateDir::new(support, ns);
        std::fs::create_dir_all(&dir.root)?;
        let store = store::open(ns, &dir.root)?;
        let (sk, pk) = keys::device_keypair();
        let api = Api::new(server);
        let body = json!({"email": email, "code": code, "device": {"name": device_name, "platform": platform(), "pubkey": b64(&pk)}});
        let v = api.call("POST", "/auth/email/finish", None, Some(&body))?;
        store::wipe(store.as_ref())?;
        store.set(store::DEVICE_KEY, &sk)?;
        let s = Session::from_tokens(dir, store, api, &v, email, device_name)?;
        Ok(s)
    }

    fn from_tokens(dir: StateDir, store: Box<dyn SecretStore>, api: Api, v: &Value, email: &str, device_name: &str) -> Result<Session> {
        let t = if v["tokens"].is_object() { &v["tokens"] } else { v };
        let refresh = str_field(t, &["refresh_token"]).ok_or_else(|| Error::Format("sign-in response has no refresh_token".into()))?;
        store.set(store::REFRESH_TOKEN, refresh.as_bytes())?;
        if let Some(a) = str_field(t, &["access_token"]) {
            let exp = now() + t["expires_in"].as_u64().unwrap_or(900);
            store.set(store::ACCESS_TOKEN, json!({"token": a, "exp": exp}).to_string().as_bytes())?;
        }
        let account_id = str_field(v, &["account_id"]).or_else(|| str_field(&v["account"], &["account_id", "id"])).unwrap_or("").to_string();
        let device_id = str_field(v, &["device_id"]).or_else(|| str_field(&v["device"], &["device_id", "id"])).unwrap_or("").to_string();
        let cfg = AccountConfig {
            server: api.base.clone(),
            account_id,
            device_id,
            device_name: device_name.to_string(),
            email: email.to_string(),
            signed_in_at: now(),
            ..Default::default()
        };
        dir.save_account(&cfg)?;
        let s = Session { dir, store, api, cfg: std::sync::Mutex::new(cfg) };
        if let Ok(me) = s.me() {
            s.update_cfg(|c| {
                if c.account_id.is_empty() {
                    c.account_id = str_field(&me, &["account_id", "id"]).unwrap_or("").to_string();
                }
                c.display_name = str_field(&me, &["display_name"]).unwrap_or("").to_string();
            })?;
        }
        Ok(s)
    }

    pub fn cfg(&self) -> AccountConfig {
        self.cfg.lock().unwrap().clone()
    }

    /// Re-reads account.json, applies `f`, writes it back (the CLI and a running game may both change it).
    pub fn update_cfg(&self, f: impl FnOnce(&mut AccountConfig)) -> Result<AccountConfig> {
        let mut g = self.cfg.lock().unwrap();
        let mut c = self.dir.load_account()?.unwrap_or_else(|| g.clone());
        f(&mut c);
        self.dir.save_account(&c)?;
        *g = c.clone();
        Ok(c)
    }

    fn token(&self) -> Result<String> {
        if let Some(b) = self.store.get(store::ACCESS_TOKEN)? {
            if let Ok(v) = serde_json::from_slice::<Value>(&b) {
                if v["exp"].as_u64().unwrap_or(0) > now() + 30 {
                    if let Some(t) = v["token"].as_str() {
                        return Ok(t.to_string());
                    }
                }
            }
        }
        self.refresh()
    }

    /// Exchanges the refresh token (rotated: the server returns a new one, stored at once).
    pub fn refresh(&self) -> Result<String> {
        let rt = self.store.get(store::REFRESH_TOKEN)?.ok_or(Error::NotSignedIn)?;
        let rt = String::from_utf8_lossy(&rt).to_string();
        let v = self.api.call("POST", "/auth/refresh", None, Some(&json!({ "refresh_token": rt })))?;
        let t = if v["tokens"].is_object() { &v["tokens"] } else { &v };
        let access = str_field(t, &["access_token"]).ok_or_else(|| Error::Format("refresh response has no access_token".into()))?.to_string();
        if let Some(r) = str_field(t, &["refresh_token"]) {
            self.store.set(store::REFRESH_TOKEN, r.as_bytes())?;
        }
        let exp = now() + t["expires_in"].as_u64().unwrap_or(900);
        self.store.set(store::ACCESS_TOKEN, json!({"token": access, "exp": exp}).to_string().as_bytes())?;
        Ok(access)
    }

    /// An authenticated request; a 401 refreshes the access token and retries once.
    pub fn call(&self, method: &str, path: &str, body: Option<&Value>) -> Result<Value> {
        let t = self.token()?;
        match self.api.call(method, path, Some(&t), body) {
            Err(Error::Unauthorized) => {
                let t = self.refresh()?;
                self.api.call(method, path, Some(&t), body)
            }
            r => r,
        }
    }

    pub fn me(&self) -> Result<Value> {
        let v = self.call("GET", "/me", None)?;
        if let Ok(p) = serde_json::from_value::<PlanInfo>(v["plan"].clone()) {
            if v["plan"].is_object() {
                let _ = self.update_cfg(|c| c.plan = Some(p));
            }
        }
        Ok(v)
    }

    /// The account save key: cached in the secret store, else fetched wrapped for this device (`GET /keys/mine`) and unwrapped.
    pub fn account_key(&self) -> Result<[u8; 32]> {
        if let Some(k) = self.store.get(store::ACCOUNT_KEY)? {
            if let Ok(k) = <[u8; 32]>::try_from(k.as_slice()) {
                return Ok(k);
            }
        }
        let v = match self.call("GET", "/keys/mine", None) {
            Err(Error::Http { status: 404, .. }) => return Err(Error::NoAccountKey),
            r => r?,
        };
        let Some(w) = str_field(&v, &["wrapped_account_key"]) else { return Err(Error::NoAccountKey) };
        let w = unb64(w).ok_or_else(|| Error::Format("wrapped_account_key is not base64".into()))?;
        let sk = self.store.get(store::DEVICE_KEY)?.ok_or_else(|| Error::Crypto("this device's key is missing: sign in again".into()))?;
        let k = keys::unwrap_account_key(&sk, &w)?;
        self.store.set(store::ACCOUNT_KEY, &k)?;
        Ok(k)
    }

    fn device_pub(&self) -> Result<[u8; 32]> {
        let sk = self.store.get(store::DEVICE_KEY)?.ok_or_else(|| Error::Crypto("this device's key is missing: sign in again".into()))?;
        keys::device_public(&sk)
    }

    fn grant(&self, device_id: &str, pubkey: &[u8], key: &[u8; 32]) -> Result<()> {
        let w = keys::wrap_account_key(key, pubkey)?;
        self.call("POST", "/keys/grant", Some(&json!({"device_id": device_id, "wrapped_account_key": b64(&w)})))?;
        Ok(())
    }

    /// After sign-in: get the account save key for this device, or make it when this is the account's first device.
    /// First device = no other device on the account and no recovery record (an account that already has a key never gets a second one).
    pub fn setup_keys(&self, recovery_cost: (u32, u32, u32)) -> Result<KeySetup> {
        match self.account_key() {
            Ok(_) => return Ok(KeySetup::Ready),
            Err(Error::NoAccountKey) => {}
            Err(e) => return Err(e),
        }
        let me = self.cfg().device_id;
        let devices = self.call("GET", "/devices", None)?;
        let others = list(&devices, "devices").iter().filter(|d| str_field(d, &["device_id", "id"]) != Some(me.as_str())).count();
        let has_recovery = match self.call("GET", "/keys/recovery", None) {
            Ok(v) => !v.is_null() && v["wrapped"].is_string(),
            Err(Error::Http { status: 404, .. }) => false,
            Err(e) => return Err(e),
        };
        if others > 0 || has_recovery {
            return Ok(KeySetup::WaitingApproval);
        }
        let key: [u8; 32] = crate::util::random();
        self.grant(&me, &self.device_pub()?, &key)?;
        let (phrase, entropy) = keys::recovery_phrase();
        let rec = keys::recovery_wrap(&key, &entropy, recovery_cost)?;
        self.call("POST", "/keys/recovery", Some(&serde_json::to_value(&rec)?))?;
        self.store.set(store::ACCOUNT_KEY, &key)?;
        Ok(KeySetup::CreatedFirst { recovery_phrase: phrase })
    }

    /// Devices waiting for the account save key (`GET /keys/requests`).
    pub fn key_requests(&self) -> Result<Vec<Value>> {
        Ok(list(&self.call("GET", "/keys/requests", None)?, "requests").to_vec())
    }

    /// Wraps the account save key to pending devices (all of them, or the one named). Returns the device ids approved.
    pub fn approve(&self, only: Option<&str>) -> Result<Vec<String>> {
        let key = self.account_key()?;
        let mut done = Vec::new();
        for r in self.key_requests()? {
            let Some(id) = str_field(&r, &["device_id", "id"]) else { continue };
            if only.is_some_and(|o| o != id) {
                continue;
            }
            let pk = str_field(&r, &["pubkey"]).and_then(unb64).ok_or_else(|| Error::Format(format!("device {id} has no public key")))?;
            self.grant(id, &pk, &key)?;
            done.push(id.to_string());
        }
        Ok(done)
    }

    /// Unlocks this device with the recovery code (and grants it the key, so later launches use `/keys/mine`).
    pub fn recover(&self, phrase: &str) -> Result<()> {
        let entropy = keys::recovery_entropy(phrase)?;
        let v = self.call("GET", "/keys/recovery", None)?;
        let rec: keys::RecoveryRecord = serde_json::from_value(v)?;
        let key = keys::recovery_unwrap(&rec, &entropy)?;
        let me = self.cfg().device_id;
        self.grant(&me, &self.device_pub()?, &key)?;
        self.store.set(store::ACCOUNT_KEY, &key)?;
        Ok(())
    }

    pub fn profiles(&self) -> Result<Vec<Value>> {
        Ok(list(&self.call("GET", "/profiles", None)?, "profiles").to_vec())
    }

    pub fn create_profile(&self, nickname: &str) -> Result<String> {
        let v = self.call("POST", "/profiles", Some(&json!({"nickname": nickname, "icon_id": 0})))?;
        let p = if v["profile"].is_object() { &v["profile"] } else { &v };
        str_field(p, &["profile_id", "id"]).map(str::to_string).ok_or_else(|| Error::Format("profile response has no profile_id".into()))
    }

    /// Links local user `uid_hex` to a profile: the given one, else the existing link, else the account's first profile (made if there is none).
    pub fn link(&self, uid_hex: &str, profile: Option<&str>) -> Result<String> {
        let pid = match profile {
            Some(p) => p.to_string(),
            None => {
                if let Some(p) = self.cfg().links.get(uid_hex) {
                    return Ok(p.clone());
                }
                let ps = self.profiles()?;
                match ps.first().and_then(|p| str_field(p, &["profile_id", "id"])) {
                    Some(p) => p.to_string(),
                    None => {
                        let c = self.cfg();
                        let nick = if c.display_name.is_empty() { "Player 1".to_string() } else { c.display_name.clone() };
                        self.create_profile(&nick)?
                    }
                }
            }
        };
        if !crate::util::valid_id(&pid) {
            return Err(Error::Format(format!("bad profile id {pid:?}")));
        }
        self.update_cfg(|c| {
            c.links.insert(uid_hex.to_string(), pid.clone());
        })?;
        Ok(pid)
    }

    /// Signs out: tells the server (best effort), removes the secrets and the sync state. Local saves and restore points stay.
    pub fn logout(self) -> Result<()> {
        if let Ok(Some(rt)) = self.store.get(store::REFRESH_TOKEN) {
            let _ = self.call("POST", "/auth/logout", Some(&json!({"refresh_token": String::from_utf8_lossy(&rt)})));
        }
        store::wipe(self.store.as_ref())?;
        self.dir.clear_account()
    }

    /// The checkout page for Napple Plus (`plan.upgrade_url`, else `<server>/billing/checkout`).
    pub fn upgrade_url(&self) -> String {
        let me = self.me().ok();
        me.as_ref()
            .and_then(|m| m["plan"]["upgrade_url"].as_str().map(str::to_string))
            .or_else(|| self.cfg().plan.and_then(|p| p.upgrade_url))
            .unwrap_or_else(|| format!("{}/billing/checkout", self.api.base))
    }
}
