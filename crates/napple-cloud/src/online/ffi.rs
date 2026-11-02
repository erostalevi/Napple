//! C API of the Napple Online client core (runtime/src/online/ is the caller). JSON in, JSON out; all calls block, so the runtime runs them
//! on a worker thread.
use super::client::Client;
use std::ffi::{c_char, c_int, CStr, CString};
use std::path::PathBuf;
use std::sync::{Arc, Mutex};

static CLIENT: Mutex<Option<Arc<Client>>> = Mutex::new(None);

fn get() -> Option<Arc<Client>> {
    CLIENT.lock().unwrap().clone()
}

unsafe fn cstr(p: *const c_char) -> String {
    if p.is_null() { String::new() } else { unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned() }
}

fn out(s: String) -> *mut c_char {
    CString::new(s.replace('\0', "")).unwrap().into_raw()
}

/// Opens the client. home NULL = NAPPLE_HOME or ~/Library/Application Support/Napple; the URLs NULL = environment / community server.
/// uid_hex: the local Switch user (32 hex digits) the profile mapping is for, or NULL.
#[unsafe(no_mangle)]
pub extern "C" fn napple_online_open(home: *const c_char, account_url: *const c_char, social_url: *const c_char, uid_hex: *const c_char) -> c_int {
    let (home, acc, soc, uid) = unsafe { (cstr(home), cstr(account_url), cstr(social_url), cstr(uid_hex)) };
    let home = if home.is_empty() { super::creds::home() } else { PathBuf::from(home) };
    let c = if acc.is_empty() && soc.is_empty() { Client::from_env(home) } else { Client::new(home, if acc.is_empty() { &soc } else { &acc }, if soc.is_empty() { &acc } else { &soc }) };
    c.set_local_user(&uid);
    *CLIENT.lock().unwrap() = Some(Arc::new(c));
    1
}

#[unsafe(no_mangle)]
pub extern "C" fn napple_online_close() {
    *CLIENT.lock().unwrap() = None;
}

/// Runs an action (name + JSON args). Returns the napple_online_result; *json_out gets malloc-style JSON to free with napple_online_free.
#[unsafe(no_mangle)]
pub extern "C" fn napple_online_act(action: *const c_char, args_json: *const c_char, json_out: *mut *mut c_char) -> c_int {
    let (a, args) = unsafe { (cstr(action), cstr(args_json)) };
    let (code, v) = match get() {
        Some(c) => c.act(&a, &serde_json::from_str(&args).unwrap_or(serde_json::Value::Null)),
        None => (6, serde_json::json!({"reason": "online client not open"})),
    };
    if !json_out.is_null() {
        unsafe { *json_out = out(v.to_string()) };
    }
    code
}

/// Re-reads everything from the servers (blocking) and returns 1 when the state changed.
#[unsafe(no_mangle)]
pub extern "C" fn napple_online_refresh() -> c_int {
    get().map_or(0, |c| c.refresh_state() as c_int)
}

/// The cached state JSON (see client.rs build_state). Free with napple_online_free.
#[unsafe(no_mangle)]
pub extern "C" fn napple_online_state() -> *mut c_char {
    out(get().map_or(serde_json::json!({"signed_in": false}), |c| c.state()).to_string())
}

/// A valid access token written to out (no NUL): its length, 0 = signed out. This is what the friend service of the game glue asks for.
#[unsafe(no_mangle)]
pub extern "C" fn napple_online_access_token(out: *mut c_char, n: usize) -> usize {
    let Some(t) = get().and_then(|c| c.access_token()) else { return 0 };
    if t.len() > n || out.is_null() {
        return 0;
    }
    unsafe { std::ptr::copy_nonoverlapping(t.as_ptr(), out as *mut u8, t.len()) };
    t.len()
}

/// The Napple profile id (UUID) the local user plays as, NUL-terminated into out; 0 = none yet.
#[unsafe(no_mangle)]
pub extern "C" fn napple_online_profile_id(out: *mut c_char, n: usize) -> usize {
    let Some(p) = get().and_then(|c| c.profile_id()) else { return 0 };
    if p.len() + 1 > n || out.is_null() {
        return 0;
    }
    unsafe {
        std::ptr::copy_nonoverlapping(p.as_ptr(), out as *mut u8, p.len());
        *out.add(p.len()) = 0;
    }
    p.len()
}

#[unsafe(no_mangle)]
pub extern "C" fn napple_online_free(p: *mut c_char) {
    if !p.is_null() {
        drop(unsafe { CString::from_raw(p) });
    }
}
