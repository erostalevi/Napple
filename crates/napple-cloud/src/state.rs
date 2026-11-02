//! Local client state: `<support dir>/cloud/<namespace>/`
//!
//! - `account.json`   server, account/device ids, per-title toggles, local user -> profile links
//! - `sync/<TITLE>-<profile>.json`  version bookkeeping per (title, profile): the version the local save is based on and the hash of
//!   that state (a different local hash = unsynced local changes), status (synced/pending/conflict/quota/error)
//! - `queue.json`     pushes that failed for a transient reason (offline): retried at the next commit, launch or `napple saves push`
//! - `restore/<TITLE>/<profile>/<time>-<reason>-<sha8>.tar.zst`  local restore points (a local save replaced by a newer cloud save)
//! - `status/<TITLE>.json` + `events.jsonl`  the last event per title and a log, for the frontend (Studio/iOS UI) and the CLI
//! - `lock`           flock held during a pull or push (the CLI and a running game share the state)
use crate::util::now;
use crate::{Error, Result};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

pub const DEFAULT_SERVER: &str = "https://pretend.napple.lat/account/v1";

/// Where Napple keeps per-user data: `$NAPPLE_SUPPORT_DIR`, else `~/Library/Application Support/Napple` (the runtime's support dir).
pub fn default_support_dir() -> PathBuf {
    if let Ok(d) = std::env::var("NAPPLE_SUPPORT_DIR") {
        if !d.is_empty() {
            return PathBuf::from(d);
        }
    }
    let home = std::env::var("HOME").unwrap_or_else(|_| ".".into());
    if cfg!(target_os = "macos") {
        PathBuf::from(home).join("Library/Application Support/Napple")
    } else {
        PathBuf::from(home).join(".local/share/napple")
    }
}

/// `NAPPLE_ACCOUNT_NAMESPACE` (default "default"): a separate sign-in, device key and state on the same machine.
pub fn namespace() -> String {
    let ns = std::env::var("NAPPLE_ACCOUNT_NAMESPACE").unwrap_or_default();
    if crate::util::valid_id(&ns) { ns } else { "default".into() }
}

#[derive(Debug, Clone, Serialize, Deserialize, Default, PartialEq)]
pub struct TitleCfg {
    pub enabled: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default, PartialEq)]
pub struct AccountConfig {
    pub server: String,
    pub account_id: String,
    pub device_id: String,
    #[serde(default)]
    pub device_name: String,
    #[serde(default)]
    pub email: String,
    #[serde(default)]
    pub display_name: String,
    /// Title id -> toggle. Titles not listed are enabled (default on after sign-in).
    #[serde(default)]
    pub titles: BTreeMap<String, TitleCfg>,
    /// Local user id (32 hex digits, the acc:u0 uid) -> Napple profile id.
    #[serde(default)]
    pub links: BTreeMap<String, String>,
    #[serde(default)]
    pub signed_in_at: u64,
}

impl AccountConfig {
    pub fn title_enabled(&self, title: &str) -> bool {
        self.titles.get(title).is_none_or(|t| t.enabled)
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, Default, PartialEq)]
pub struct SyncState {
    /// The cloud version the local save is based on (0 = never synced).
    pub version: u64,
    /// sha256 of the tar of that state.
    pub plain_sha256: String,
    #[serde(default)]
    pub cipher_sha256: String,
    /// synced | pending | conflict | quota | error
    #[serde(default)]
    pub status: String,
    #[serde(default)]
    pub detail: String,
    #[serde(default)]
    pub remote_head: u64,
    #[serde(default)]
    pub updated_at: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct QueueItem {
    pub title: String,
    pub profile: String,
    pub uid: String,
    pub save_dir: String,
    pub enqueued_at: u64,
    #[serde(default)]
    pub attempts: u32,
    #[serde(default)]
    pub last_error: String,
}

#[derive(Debug, Clone)]
pub struct StateDir {
    pub root: PathBuf,
}

type Hook = Box<dyn Fn(&serde_json::Value) + Send + Sync>;
static EVENT_HOOK: Mutex<Option<Hook>> = Mutex::new(None);

/// Called with every event (the runtime logs it and passes it to the frontend).
pub fn set_event_hook(f: Option<Hook>) {
    *EVENT_HOOK.lock().unwrap() = f;
}

pub fn read_json<T: for<'de> Deserialize<'de>>(p: &Path) -> Result<Option<T>> {
    match std::fs::read(p) {
        Ok(b) => Ok(Some(serde_json::from_slice(&b).map_err(|e| Error::Format(format!("{}: {e}", p.display())))?)),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(e.into()),
    }
}

pub fn write_json<T: Serialize>(p: &Path, v: &T) -> Result<()> {
    if let Some(d) = p.parent() {
        std::fs::create_dir_all(d)?;
    }
    let tmp = p.with_extension(format!("tmp{}", std::process::id()));
    std::fs::write(&tmp, serde_json::to_vec_pretty(v)?)?;
    std::fs::rename(&tmp, p)?;
    Ok(())
}

pub struct Lock(std::fs::File);
impl Drop for Lock {
    fn drop(&mut self) {
        use std::os::fd::AsRawFd;
        unsafe { libc::flock(self.0.as_raw_fd(), libc::LOCK_UN) };
    }
}

impl StateDir {
    pub fn new(support: &Path, ns: &str) -> Self {
        StateDir { root: support.join("cloud").join(ns) }
    }
    pub fn account_path(&self) -> PathBuf {
        self.root.join("account.json")
    }
    pub fn load_account(&self) -> Result<Option<AccountConfig>> {
        read_json(&self.account_path())
    }
    pub fn save_account(&self, c: &AccountConfig) -> Result<()> {
        write_json(&self.account_path(), c)
    }
    fn sync_path(&self, title: &str, profile: &str) -> PathBuf {
        self.root.join("sync").join(format!("{title}-{profile}.json"))
    }
    pub fn load_sync(&self, title: &str, profile: &str) -> SyncState {
        read_json(&self.sync_path(title, profile)).ok().flatten().unwrap_or_default()
    }
    pub fn save_sync(&self, title: &str, profile: &str, s: &SyncState) -> Result<()> {
        let mut s = s.clone();
        s.updated_at = now();
        write_json(&self.sync_path(title, profile), &s)
    }
    pub fn all_sync(&self) -> Vec<(String, String, SyncState)> {
        let mut out = Vec::new();
        if let Ok(rd) = std::fs::read_dir(self.root.join("sync")) {
            for e in rd.flatten() {
                let n = e.file_name().to_string_lossy().to_string();
                let Some(stem) = n.strip_suffix(".json") else { continue };
                let Some((t, p)) = stem.split_once('-') else { continue };
                if let Ok(Some(s)) = read_json::<SyncState>(&e.path()) {
                    out.push((t.to_string(), p.to_string(), s));
                }
            }
        }
        out.sort_by(|a, b| (&a.0, &a.1).cmp(&(&b.0, &b.1)));
        out
    }
    pub fn load_queue(&self) -> Vec<QueueItem> {
        read_json(&self.root.join("queue.json")).ok().flatten().unwrap_or_default()
    }
    pub fn enqueue(&self, item: QueueItem) -> Result<()> {
        let mut q = self.load_queue();
        let attempts = q.iter().find(|i| i.title == item.title && i.profile == item.profile).map_or(0, |i| i.attempts);
        q.retain(|i| !(i.title == item.title && i.profile == item.profile));
        q.push(QueueItem { attempts: attempts + 1, ..item });
        write_json(&self.root.join("queue.json"), &q)
    }
    pub fn dequeue(&self, title: &str, profile: &str) -> Result<()> {
        let mut q = self.load_queue();
        let n = q.len();
        q.retain(|i| !(i.title == title && i.profile == profile));
        if q.len() != n { write_json(&self.root.join("queue.json"), &q) } else { Ok(()) }
    }
    pub fn restore_dir(&self, title: &str, profile: &str) -> PathBuf {
        self.root.join("restore").join(title).join(profile)
    }
    /// Keeps `tar` as a restore point (deduplicated by content: the same save is kept once).
    pub fn save_restore_point(&self, title: &str, profile: &str, reason: &str, tar: &[u8]) -> Result<PathBuf> {
        let d = self.restore_dir(title, profile);
        std::fs::create_dir_all(&d)?;
        let sha8 = &crate::util::sha256_hex(tar)[..16];
        for (p, _) in self.restore_points(title, profile) {
            if p.file_name().is_some_and(|n| n.to_string_lossy().ends_with(&format!("-{sha8}.tar.zst"))) {
                return Ok(p);
            }
        }
        let p = d.join(format!("{}-{reason}-{sha8}.tar.zst", now()));
        let z = zstd::bulk::compress(tar, 9).map_err(|e| Error::Format(format!("zstd: {e}")))?;
        std::fs::write(&p, z)?;
        Ok(p)
    }
    pub fn restore_points(&self, title: &str, profile: &str) -> Vec<(PathBuf, u64)> {
        let mut v: Vec<(PathBuf, u64)> = std::fs::read_dir(self.restore_dir(title, profile))
            .map(|rd| {
                rd.flatten()
                    .filter(|e| e.file_name().to_string_lossy().ends_with(".tar.zst"))
                    .map(|e| (e.path(), e.metadata().map(|m| m.len()).unwrap_or(0)))
                    .collect()
            })
            .unwrap_or_default();
        v.sort();
        v
    }
    pub fn read_restore_point(p: &Path) -> Result<Vec<u8>> {
        let z = std::fs::read(p)?;
        zstd::bulk::decompress(&z, crate::tarball::MAX_TOTAL as usize + (1 << 20)).map_err(|e| Error::Format(format!("zstd: {e}")))
    }
    /// Records an event: `status/<TITLE>.json` (latest), `events.jsonl` (log), and the in-process hook.
    pub fn event(&self, title: &str, profile: &str, kind: &str, message: &str, extra: serde_json::Value) {
        let mut v = serde_json::json!({"time": now(), "title_id": title, "profile_id": profile, "event": kind, "message": message});
        if let (Some(o), serde_json::Value::Object(x)) = (v.as_object_mut(), extra) {
            o.extend(x);
        }
        let _ = write_json(&self.root.join("status").join(format!("{title}.json")), &v);
        if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(self.root.join("events.jsonl")) {
            use std::io::Write;
            let _ = writeln!(f, "{v}");
        }
        if let Some(h) = EVENT_HOOK.lock().unwrap().as_ref() {
            h(&v);
        }
    }
    pub fn lock(&self) -> Result<Lock> {
        use std::os::fd::AsRawFd;
        std::fs::create_dir_all(&self.root)?;
        let f = std::fs::OpenOptions::new().create(true).truncate(false).write(true).open(self.root.join("lock"))?;
        if unsafe { libc::flock(f.as_raw_fd(), libc::LOCK_EX) } != 0 {
            return Err(std::io::Error::last_os_error().into());
        }
        Ok(Lock(f))
    }
    /// Removes the sign-in state (keeps restore points: they are the user's saves).
    pub fn clear_account(&self) -> Result<()> {
        for p in ["account.json", "queue.json"] {
            let _ = std::fs::remove_file(self.root.join(p));
        }
        let _ = std::fs::remove_dir_all(self.root.join("sync"));
        let _ = std::fs::remove_dir_all(self.root.join("status"));
        let _ = std::fs::remove_dir_all(self.root.join("secrets"));
        Ok(())
    }
}
