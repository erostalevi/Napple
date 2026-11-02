//! The sync engine: pull before mount, push after commit, conflicts, plan limit, offline queue, history and restore.
//!
//! Version bookkeeping per (title, profile) (`state::SyncState`): `version` is the cloud version the local save is based on and
//! `plain_sha256` the hash of that state. Comparing the current local hash with it tells whether the local save has unsynced changes.
//! - pull: remote head newer than `version` -> download, decrypt, install. If the local save also changed (or this device never synced
//!   this title and has a different save), the local copy is kept as a restore point first and a `conflict` event tells the frontend:
//!   the remote becomes current, nothing is lost.
//! - push: new version = `version + 1` with `base_version = version`. 409 (someone else pushed first): the local copy is kept as a
//!   restore point, status `conflict`; the next launch's pull installs the remote. 402: status `plan_limit` with the upgrade URL, the
//!   save stays local. Offline/5xx: queued, retried later. Never deletes or overwrites a local save that is not in the cloud or a restore point.
use crate::api::{list, str_field};
use crate::blob;
use crate::savedir;
use crate::session::Session;
use crate::state::{QueueItem, SyncState};
use crate::tarball;
use crate::util::{now, sha256_hex};
use crate::{Error, Result};
use serde_json::{json, Value};
use std::path::{Path, PathBuf};
use std::time::Instant;

#[derive(Debug, Clone, PartialEq)]
pub struct Target {
    pub title: String,
    pub profile: String,
    /// Local user id, 32 hex digits (acc:u0 uid).
    pub uid: String,
    /// `<save root>/<TITLE>/save`
    pub save_dir: PathBuf,
}

#[derive(Debug, Clone, PartialEq)]
pub enum PullResult {
    Disabled,
    UpToDate,
    /// Nothing in the cloud for this title/profile yet.
    NoRemote,
    /// The local save has changes the cloud does not have (a push follows).
    LocalNewer,
    Pulled { version: u64, restore_point: Option<PathBuf> },
}

#[derive(Debug, Clone, PartialEq)]
pub enum PushResult {
    Disabled,
    NothingLocal,
    Unchanged,
    Pushed { version: u64 },
    Conflict { head: u64, restore_point: PathBuf },
    PlanLimit { upgrade_url: String },
    Queued { error: String },
}

pub fn uid_hex(uid: &[u8; 16]) -> String {
    crate::util::hex(uid)
}

/// The runtime's first local user (runtime/src/services/account.c g_user_id): the user `napple account login` links by default.
pub const DEFAULT_UID: &str = "4e617070706c65557365720000000001";

/// The (title, profile) targets for the local users that are linked to a profile.
pub fn targets(s: &Session, title: &str, save_dir: &Path, uids: &[String]) -> Vec<Target> {
    let cfg = s.cfg();
    uids.iter()
        .filter_map(|u| {
            cfg.links.get(u).map(|p| Target { title: title.to_string(), profile: p.clone(), uid: u.clone(), save_dir: save_dir.to_path_buf() })
        })
        .collect()
}

fn versions_path(t: &Target) -> String {
    format!("/saves/{}/{}/versions", t.title, t.profile)
}

/// The versions of a title/profile (oldest first) with the committed head.
pub fn versions(s: &Session, t: &Target) -> Result<(Vec<Value>, u64)> {
    let v = match s.call("GET", &versions_path(t), None) {
        Err(Error::Http { status: 404, .. }) => return Ok((Vec::new(), 0)),
        r => r?,
    };
    let mut vs: Vec<Value> = list(&v, "versions").iter().filter(|x| x["committed"].as_bool() != Some(false)).cloned().collect();
    vs.sort_by_key(|x| x["version"].as_u64().unwrap_or(0));
    let head = vs.last().and_then(|x| x["version"].as_u64()).unwrap_or(0);
    Ok((vs, head))
}

fn download(s: &Session, t: &Target, version: u64, expect_sha: Option<&str>) -> Result<Vec<u8>> {
    let v = s.call("GET", &format!("{}/{}", versions_path(t), version), None)?;
    let url = str_field(&v, &["url", "download_url", "get_url"]).ok_or_else(|| Error::Format("version response has no download url".into()))?;
    let b = s.api.get_bytes(url, blob::MAX_BLOB)?;
    if let Some(e) = expect_sha {
        if !e.is_empty() && !sha256_hex(&b).eq_ignore_ascii_case(e) {
            return Err(Error::Format(format!("version {version}: downloaded data does not match its sha256")));
        }
    }
    Ok(b)
}

/// Decrypted tar of a cloud version.
pub fn fetch_version(s: &Session, t: &Target, version: u64, versions: &[Value]) -> Result<Vec<u8>> {
    let key = s.account_key()?;
    let sha = versions.iter().find(|x| x["version"].as_u64() == Some(version)).and_then(|x| x["sha256_ciphertext"].as_str());
    let b = download(s, t, version, sha)?;
    blob::open(&key, &t.title, &t.profile, version, &b)
}

/// Pull before mount. With a deadline, gives up (touching nothing) when it passes: the game then starts from the local save.
pub fn pull(s: &Session, t: &Target, deadline: Option<Instant>) -> Result<PullResult> {
    if !s.cfg().title_enabled(&t.title) {
        return Ok(PullResult::Disabled);
    }
    let _l = s.dir.lock()?;
    s.api.set_deadline(deadline);
    let r = pull_locked(s, t, deadline);
    s.api.set_deadline(None);
    r
}

fn pull_locked(s: &Session, t: &Target, deadline: Option<Instant>) -> Result<PullResult> {
    let mut st = s.dir.load_sync(&t.title, &t.profile);
    let (vs, head) = versions(s, t)?;
    let local = savedir::local_tar(&t.save_dir, &t.uid)?;
    let local_sha = local.as_deref().map(sha256_hex);
    let local_changed = local_sha.as_ref().is_some_and(|l| *l != st.plain_sha256);
    if head == 0 {
        return Ok(if local.is_some() { PullResult::LocalNewer } else { PullResult::NoRemote });
    }
    if head <= st.version {
        if head < st.version {
            // the cloud lost versions (restored backup, deleted data): continue from its head
            st.version = head;
            st.plain_sha256.clear();
            s.dir.save_sync(&t.title, &t.profile, &st)?;
            return Ok(PullResult::LocalNewer);
        }
        return Ok(if local_changed { PullResult::LocalNewer } else { PullResult::UpToDate });
    }
    let tar = fetch_version(s, t, head, &vs)?;
    let entries = tarball::parse(&tar)?;
    let remote_sha = sha256_hex(&tar);
    if deadline.is_some_and(|d| Instant::now() > d) {
        return Err(Error::Timeout);
    }
    let mut restore_point = None;
    if local_sha.as_deref() == Some(remote_sha.as_str()) {
        // already identical (pushed from here, or the same save made twice)
    } else {
        if local_changed {
            let p = s.dir.save_restore_point(&t.title, &t.profile, "local", local.as_deref().unwrap())?;
            s.dir.event(
                &t.title,
                &t.profile,
                "conflict",
                &format!("the cloud had a newer save (version {head}); this device's save was kept as a restore point"),
                json!({"version": head, "restore_point": p.display().to_string()}),
            );
            restore_point = Some(p);
        }
        savedir::install(&t.save_dir, &t.uid, &entries)?;
    }
    s.dir.save_sync(
        &t.title,
        &t.profile,
        &SyncState { version: head, plain_sha256: remote_sha, status: "synced".into(), remote_head: head, ..Default::default() },
    )?;
    s.dir.event(&t.title, &t.profile, "pulled", &format!("save version {head} from the cloud"), json!({"version": head}));
    Ok(PullResult::Pulled { version: head, restore_point })
}

/// Push the committed local save. `force` retries even right after a plan-limit answer.
pub fn push(s: &Session, t: &Target, force: bool) -> Result<PushResult> {
    if !s.cfg().title_enabled(&t.title) {
        return Ok(PushResult::Disabled);
    }
    let _l = s.dir.lock()?;
    let Some(tar) = savedir::local_tar(&t.save_dir, &t.uid)? else { return Ok(PushResult::NothingLocal) };
    let local_sha = sha256_hex(&tar);
    push_tar(s, t, tar, &local_sha, force)
}

fn push_tar(s: &Session, t: &Target, tar: Vec<u8>, local_sha: &str, force: bool) -> Result<PushResult> {
    let mut st = s.dir.load_sync(&t.title, &t.profile);
    if st.version > 0 && st.plain_sha256 == local_sha {
        s.dir.dequeue(&t.title, &t.profile)?;
        return Ok(PushResult::Unchanged);
    }
    if !force && st.status == "plan_limit" && st.updated_at + 3600 > now() {
        return Ok(PushResult::PlanLimit { upgrade_url: st.upgrade_url.clone().unwrap_or_default() });
    }
    let key = s.account_key()?;
    let version = st.version + 1;
    let data = blob::seal(&key, &t.title, &t.profile, version, &tar)?;
    let cipher_sha = sha256_hex(&data);
    let body = json!({"base_version": st.version, "size": data.len(), "sha256_ciphertext": cipher_sha});
    let r = s.call("POST", &versions_path(t), Some(&body)).and_then(|v| {
        let got = v["version"].as_u64().unwrap_or(0);
        if got != version {
            return Err(Error::Format(format!("server assigned version {got}, expected {version} (base_version + 1)")));
        }
        let url = str_field(&v, &["upload_url", "url"]).ok_or_else(|| Error::Format("no upload_url".into()))?.to_string();
        s.api.put_bytes(&url, &data)?;
        s.call("POST", &format!("{}/{}/commit", versions_path(t), version), Some(&json!({})))?;
        Ok(())
    });
    match r {
        Ok(()) => {
            st = SyncState {
                version,
                plain_sha256: local_sha.to_string(),
                cipher_sha256: cipher_sha,
                status: "synced".into(),
                remote_head: version,
                ..Default::default()
            };
            s.dir.save_sync(&t.title, &t.profile, &st)?;
            s.dir.dequeue(&t.title, &t.profile)?;
            s.dir.event(&t.title, &t.profile, "pushed", &format!("save version {version} uploaded"), json!({"version": version}));
            Ok(PushResult::Pushed { version })
        }
        Err(Error::Conflict { head }) => {
            let p = s.dir.save_restore_point(&t.title, &t.profile, "local", &tar)?;
            st.status = "conflict".into();
            st.remote_head = head;
            st.detail = format!("the cloud has version {head} from another device; this save is kept as a restore point and the cloud save is used from the next launch");
            s.dir.save_sync(&t.title, &t.profile, &st)?;
            s.dir.dequeue(&t.title, &t.profile)?;
            s.dir.event(&t.title, &t.profile, "conflict", &st.detail, json!({"version": head, "restore_point": p.display().to_string()}));
            Ok(PushResult::Conflict { head, restore_point: p })
        }
        Err(Error::PlanLimit { upgrade_url }) => {
            st.status = "plan_limit".into();
            st.upgrade_url = Some(upgrade_url.clone());
            st.detail = "not synced (plan limit)".into();
            s.dir.save_sync(&t.title, &t.profile, &st)?;
            s.dir.dequeue(&t.title, &t.profile)?;
            s.dir.event(
                &t.title,
                &t.profile,
                "plan_limit",
                "not synced (plan limit): the save stays on this device; Napple Plus syncs every title",
                json!({"upgrade_url": upgrade_url}),
            );
            Ok(PushResult::PlanLimit { upgrade_url })
        }
        Err(e) if e.is_transient() => {
            st.status = "pending".into();
            st.detail = e.to_string();
            s.dir.save_sync(&t.title, &t.profile, &st)?;
            s.dir.enqueue(QueueItem {
                title: t.title.clone(),
                profile: t.profile.clone(),
                uid: t.uid.clone(),
                save_dir: t.save_dir.display().to_string(),
                enqueued_at: now(),
                attempts: 0,
                last_error: e.to_string(),
            })?;
            Ok(PushResult::Queued { error: e.to_string() })
        }
        Err(e) => {
            st.status = "error".into();
            st.detail = e.to_string();
            s.dir.save_sync(&t.title, &t.profile, &st)?;
            Err(e)
        }
    }
}

/// Retries every queued push. Returns the results per queued target.
pub fn process_queue(s: &Session) -> Vec<(Target, Result<PushResult>)> {
    s.dir
        .load_queue()
        .into_iter()
        .map(|q| {
            let t = Target { title: q.title, profile: q.profile, uid: q.uid, save_dir: PathBuf::from(q.save_dir) };
            let r = push(s, &t, false);
            (t, r)
        })
        .collect()
}

/// Puts a cloud version back as the current save (and pushes it as the new head, so other devices get it too).
pub fn restore_version(s: &Session, t: &Target, version: u64) -> Result<PushResult> {
    let (vs, head) = versions(s, t)?;
    let tar = fetch_version(s, t, version, &vs)?;
    restore_tar(s, t, tar, head, &format!("cloud version {version}"))
}

/// Puts a local restore point back as the current save (and pushes it).
pub fn restore_point(s: &Session, t: &Target, p: &Path) -> Result<PushResult> {
    let tar = crate::state::StateDir::read_restore_point(p)?;
    let (_, head) = versions(s, t)?;
    restore_tar(s, t, tar, head, &format!("restore point {}", p.display()))
}

fn restore_tar(s: &Session, t: &Target, tar: Vec<u8>, head: u64, what: &str) -> Result<PushResult> {
    let entries = tarball::parse(&tar)?;
    let _l = s.dir.lock()?;
    if let Some(cur) = savedir::local_tar(&t.save_dir, &t.uid)? {
        if cur != tar {
            s.dir.save_restore_point(&t.title, &t.profile, "before-restore", &cur)?;
        }
    }
    savedir::install(&t.save_dir, &t.uid, &entries)?;
    // based on the cloud head: the restored save becomes version head + 1
    let mut st = s.dir.load_sync(&t.title, &t.profile);
    st.version = head;
    st.plain_sha256.clear();
    st.status = "pending".into();
    s.dir.save_sync(&t.title, &t.profile, &st)?;
    s.dir.event(&t.title, &t.profile, "restored", &format!("{what} restored"), json!({}));
    let sha = sha256_hex(&tar);
    push_tar(s, t, tar, &sha, true)
}
