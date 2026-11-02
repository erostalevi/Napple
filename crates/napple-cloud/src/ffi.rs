//! C API for the native runtime (include/napple_cloud.h; runtime/src/cloud/cloud.c is the caller).
//!
//! - `napple_cloud_runtime_start` (before the game opens any save): signed out, title disabled or no linked user -> returns 0 and does nothing.
//!   Otherwise pulls each linked user's save (bounded by the timeout; offline or slow = start from the local save) and starts a background
//!   thread that pushes after commits (debounced) and retries queued pushes with backoff.
//! - `napple_cloud_runtime_commit`: a save was committed (cheap; never blocks the game thread on the network).
//! - `napple_cloud_runtime_flush`: clean exit; pushes what is pending, waiting at most the timeout.
//! - `napple_cloud_runtime_status`: JSON for the frontend; `napple_cloud_set_event_callback`: conflict / plan_limit / pulled / pushed events.
use crate::session::Session;
use crate::sync::{self, PullResult, PushResult, Target};
use crate::Error;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::path::PathBuf;
use std::sync::{Arc, Condvar, Mutex, OnceLock};
use std::time::{Duration, Instant};

pub type LogFn = extern "C" fn(*const c_char);
pub type EventFn = extern "C" fn(*const c_char, *mut c_void);

struct Ctl {
    dirty: bool,
    last_commit: Instant,
    flush_gen: u64,
    done_gen: u64,
    retry_at: Option<Instant>,
    backoff: Duration,
}

struct Rt {
    session: Session,
    targets: Vec<Target>,
    ctl: Mutex<Ctl>,
    cv: Condvar,
    debounce: Duration,
}

static RT: OnceLock<Arc<Rt>> = OnceLock::new();
static LOG: Mutex<Option<LogFn>> = Mutex::new(None);
struct Cb(EventFn, usize);
static EVENT_CB: Mutex<Option<Cb>> = Mutex::new(None);
static STATUS: Mutex<Option<CString>> = Mutex::new(None);

fn log(msg: &str) {
    let f = *LOG.lock().unwrap();
    match (f, CString::new(msg.replace('\0', " "))) {
        (Some(f), Ok(c)) => f(c.as_ptr()),
        _ => eprintln!("[cloud] {msg}"),
    }
}

unsafe fn cstr(p: *const c_char) -> Option<String> {
    if p.is_null() { None } else { unsafe { CStr::from_ptr(p) }.to_str().ok().map(str::to_string) }
}

fn env_ms(name: &str, default: u64) -> Duration {
    Duration::from_millis(std::env::var(name).ok().and_then(|s| s.parse().ok()).unwrap_or(default))
}

/// # Safety
/// Strings are NUL-terminated or NULL; `uids` is a comma-separated list of 32-hex-digit local user ids.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn napple_cloud_runtime_start(
    support_dir: *const c_char,
    save_dir: *const c_char,
    title_id: *const c_char,
    uids: *const c_char,
    pull_timeout_ms: c_int,
    log_fn: Option<LogFn>,
) -> c_int {
    *LOG.lock().unwrap() = log_fn;
    let (Some(support), Some(save_dir), Some(title), Some(uids)) =
        (unsafe { cstr(support_dir) }, unsafe { cstr(save_dir) }, unsafe { cstr(title_id) }, unsafe { cstr(uids) })
    else {
        return 0;
    };
    if RT.get().is_some() {
        return 1;
    }
    let Some(title) = crate::util::normalize_title(&title) else { return 0 };
    let session = match Session::open(&PathBuf::from(&support)) {
        Ok(s) => s,
        Err(Error::NotSignedIn) => return 0,
        Err(e) => {
            log(&format!("cloud saves off: {e}"));
            return 0;
        }
    };
    let cfg = session.cfg();
    if !cfg.title_enabled(&title) {
        log(&format!("cloud saves are turned off for {title} (napple saves enable {title})"));
        return 0;
    }
    let uids: Vec<String> = uids.split(',').map(|s| s.trim().to_ascii_lowercase()).filter(|s| s.len() == 32).collect();
    let targets = sync::targets(&session, &title, &PathBuf::from(&save_dir), &uids);
    if targets.is_empty() {
        log("cloud saves: no local user is linked to a Napple profile (napple account link)");
        return 0;
    }
    crate::state::set_event_hook(Some(Box::new(|v| {
        log(&format!("{}: {}", v["event"].as_str().unwrap_or("event"), v["message"].as_str().unwrap_or("")));
        if let Some(Cb(f, ctx)) = EVENT_CB.lock().unwrap().as_ref() {
            if let Ok(c) = CString::new(v.to_string()) {
                f(c.as_ptr(), *ctx as *mut c_void);
            }
        }
    })));
    log(&format!("cloud saves on: {} as device {} ({}), {} profile(s)", cfg.email, cfg.device_name, cfg.device_id, targets.len()));
    let timeout = Duration::from_millis(pull_timeout_ms.max(0) as u64);
    for t in &targets {
        let t0 = Instant::now();
        match sync::pull(&session, t, Some(t0 + timeout)) {
            Ok(PullResult::Pulled { version, restore_point }) => {
                log(&format!("pulled {} profile {} version {version} before mount ({} ms)", t.title, t.profile, t0.elapsed().as_millis()));
                if let Some(p) = restore_point {
                    log(&format!("this device's previous save was kept as a restore point: {}", p.display()));
                }
            }
            Ok(PullResult::UpToDate) => log(&format!("{} profile {}: up to date", t.title, t.profile)),
            Ok(PullResult::LocalNewer) | Ok(PullResult::NoRemote) => {
                log(&format!("{} profile {}: the local save is newer than the cloud: pushing it in the background", t.title, t.profile));
            }
            Ok(PullResult::Disabled) => {}
            Err(Error::NoAccountKey) => log(&format!("{}", Error::NoAccountKey)),
            Err(e) => log(&format!("pull failed, starting from the local save: {e}")),
        }
    }
    let rt = Arc::new(Rt {
        session,
        targets,
        ctl: Mutex::new(Ctl {
            dirty: true, // the first round pushes what the pull found newer locally and retries the offline queue
            last_commit: Instant::now() - Duration::from_secs(3600),
            flush_gen: 0,
            done_gen: 0,
            retry_at: None,
            backoff: Duration::from_secs(5),
        }),
        cv: Condvar::new(),
        debounce: env_ms("NAPPLE_CLOUD_DEBOUNCE_MS", 5000),
    });
    let _ = RT.set(rt.clone());
    std::thread::Builder::new().name("napple-cloud".into()).spawn(move || worker(rt)).ok();
    1
}

fn worker(rt: Arc<Rt>) {
    let mut first = true;
    loop {
        let round;
        {
            let mut c = rt.ctl.lock().unwrap();
            loop {
                if c.flush_gen > c.done_gen {
                    break;
                }
                let now = Instant::now();
                if c.dirty {
                    let due = c.last_commit + rt.debounce;
                    if now >= due {
                        break;
                    }
                    c = rt.cv.wait_timeout(c, due - now).unwrap().0;
                    continue;
                }
                if let Some(r) = c.retry_at {
                    if now >= r {
                        break;
                    }
                    c = rt.cv.wait_timeout(c, r - now).unwrap().0;
                    continue;
                }
                c = rt.cv.wait(c).unwrap();
            }
            round = c.flush_gen;
            c.dirty = false;
            c.retry_at = None;
        }
        let mut transient = false;
        for t in &rt.targets {
            match sync::push(&rt.session, t, false) {
                Ok(PushResult::Pushed { version }) => log(&format!("pushed {} profile {} version {version}", t.title, t.profile)),
                Ok(PushResult::Queued { error }) => {
                    log(&format!("offline, push queued: {error}"));
                    transient = true;
                }
                Ok(PushResult::Conflict { head, .. }) => log(&format!("conflict with cloud version {head}: local save kept as a restore point")),
                Ok(PushResult::PlanLimit { upgrade_url }) => log(&format!("not synced (plan limit); upgrade: {upgrade_url}")),
                Ok(_) => {}
                Err(e) => {
                    log(&format!("push failed: {e}"));
                    transient |= e.is_transient();
                }
            }
        }
        if first {
            first = false;
            for (t, r) in sync::process_queue(&rt.session) {
                if let Ok(PushResult::Pushed { version }) = r {
                    log(&format!("queued push of {} profile {} done (version {version})", t.title, t.profile));
                }
            }
        }
        let mut c = rt.ctl.lock().unwrap();
        c.done_gen = c.done_gen.max(round);
        if transient {
            c.retry_at = Some(Instant::now() + c.backoff);
            c.backoff = (c.backoff * 2).min(Duration::from_secs(300));
        } else {
            c.backoff = Duration::from_secs(5);
        }
        rt.cv.notify_all();
    }
}

/// A save was committed: push it after the debounce interval (NAPPLE_CLOUD_DEBOUNCE_MS, default 5000).
#[unsafe(no_mangle)]
pub extern "C" fn napple_cloud_runtime_commit() {
    if let Some(rt) = RT.get() {
        let mut c = rt.ctl.lock().unwrap();
        c.dirty = true;
        c.last_commit = Instant::now();
        rt.cv.notify_all();
    }
}

/// Pushes pending changes now and waits for them (at most `timeout_ms`). 1 = done (or nothing to do), 0 = timed out or not active.
#[unsafe(no_mangle)]
pub extern "C" fn napple_cloud_runtime_flush(timeout_ms: c_int) -> c_int {
    let Some(rt) = RT.get() else { return 0 };
    let deadline = Instant::now() + Duration::from_millis(timeout_ms.max(0) as u64);
    let mut c = rt.ctl.lock().unwrap();
    c.flush_gen += 1;
    let want = c.flush_gen;
    rt.cv.notify_all();
    while c.done_gen < want {
        let now = Instant::now();
        if now >= deadline {
            log("exit: cloud push still running, leaving it queued for the next launch");
            return 0;
        }
        c = rt.cv.wait_timeout(c, deadline - now).unwrap().0;
    }
    1
}

/// JSON: {"active": bool, "title_id", "targets": [{profile_id, status, version, detail, upgrade_url}]}. Valid until the next call.
#[unsafe(no_mangle)]
pub extern "C" fn napple_cloud_runtime_status() -> *const c_char {
    let v = match RT.get() {
        None => serde_json::json!({"active": false}),
        Some(rt) => {
            let ts: Vec<_> = rt
                .targets
                .iter()
                .map(|t| {
                    let s = rt.session.dir.load_sync(&t.title, &t.profile);
                    serde_json::json!({"profile_id": t.profile, "status": s.status, "version": s.version, "detail": s.detail, "upgrade_url": s.upgrade_url})
                })
                .collect();
            serde_json::json!({"active": true, "title_id": rt.targets.first().map(|t| t.title.clone()), "targets": ts})
        }
    };
    let mut g = STATUS.lock().unwrap();
    *g = CString::new(v.to_string()).ok();
    g.as_ref().map_or(std::ptr::null(), |c| c.as_ptr())
}

#[unsafe(no_mangle)]
pub extern "C" fn napple_cloud_set_event_callback(f: Option<EventFn>, ctx: *mut c_void) {
    *EVENT_CB.lock().unwrap() = f.map(|f| Cb(f, ctx as usize));
}
