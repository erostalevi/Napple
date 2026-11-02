//! Headless driver of the Napple Online client core for tools/e2e/online: `online_cli act <action> '<json args>' | state | token | profile`.
//! NAPPLE_HOME picks the "device"; NAPPLE_ACCOUNT_URL / NAPPLE_SOCIAL_URL the servers. Prints `<result code> <json>` for act.
use napple_cloud::ffi::{napple_cloud_runtime_commit, napple_cloud_runtime_flush, napple_cloud_runtime_start, napple_cloud_runtime_status};
use napple_cloud::online::{client::Client, creds};
use std::ffi::{CStr, CString};

fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    let c = Client::from_env(creds::home());
    if let Ok(u) = std::env::var("NAPPLE_LOCAL_USER") {
        c.set_local_user(&u);
    }
    match a.first().map(String::as_str) {
        Some("act") => {
            c.refresh_state(); // the app keeps this cache warm; a one-shot process has to fill it
            let args = a.get(2).map(|s| serde_json::from_str(s).expect("json args")).unwrap_or(serde_json::json!({}));
            let (code, v) = c.act(&a[1], &args);
            println!("{code} {v}");
        }
        Some("state") => {
            c.refresh_state();
            println!("{}", c.state());
        }
        Some("token") => println!("{}", c.access_token().unwrap_or_default()),
        Some("profile") => println!("{}", c.profile_id().unwrap_or_default()),
        // cloud <support dir> <save dir> <TITLE16HEX> <uid32hex> [push]: what the runtime does at launch (pull before mount) and, with push, after a commit
        Some("cloud") => {
            let cs = |i: usize| CString::new(a[i].clone()).unwrap();
            let on = unsafe { napple_cloud_runtime_start(cs(1).as_ptr(), cs(2).as_ptr(), cs(3).as_ptr(), cs(4).as_ptr(), 15000, None) };
            if a.get(5).map(String::as_str) == Some("push") {
                napple_cloud_runtime_commit();
                println!("flush {}", napple_cloud_runtime_flush(20000));
            }
            let st = unsafe { CStr::from_ptr(napple_cloud_runtime_status()) }.to_string_lossy().into_owned();
            println!("active {on} {st}");
        }
        _ => eprintln!("usage: online_cli act <action> '<json>' | state | token | profile | cloud <support> <save dir> <TITLE> <uid> [push]"),
    }
}
