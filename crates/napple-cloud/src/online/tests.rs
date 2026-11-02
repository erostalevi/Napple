//! The client against a fake account + social server on localhost.
use super::client::*;
use super::creds::CredStore;
use crate::util::b64;
use serde_json::{json, Value};
use std::sync::{Arc, Mutex};

type Log = Arc<Mutex<Vec<String>>>;

fn jwt(sub: &str, did: &str) -> String {
    format!("{}.{}.sig", b64(b"{\"alg\":\"EdDSA\"}"), b64(json!({"sub": sub, "did": did}).to_string().as_bytes()))
}

/// A tiny account+social server. Refresh tokens rotate (r1 -> r2 ...), a reused one gets 401.
fn serve() -> (String, Log) {
    let server = tiny_http::Server::http("127.0.0.1:0").unwrap();
    let url = format!("http://127.0.0.1:{}", server.server_addr().to_ip().unwrap().port());
    let log: Log = Default::default();
    let l = log.clone();
    std::thread::spawn(move || {
        let mut rt = 0;
        let mut verified = false;
        for mut req in server.incoming_requests() {
            let mut body = String::new();
            let _ = std::io::Read::read_to_string(req.as_reader(), &mut body);
            let v: Value = serde_json::from_str(&body).unwrap_or(Value::Null);
            let path = req.url().to_string();
            let m = req.method().to_string();
            let prof = req.headers().iter().find(|h| h.field.equiv("X-Napple-Profile")).map(|h| h.value.to_string());
            l.lock().unwrap().push(format!("{m} {path} profile={}", prof.unwrap_or_default()));
            let (code, out): (u16, Value) = match (m.as_str(), path.split('?').next().unwrap()) {
                ("POST", "/account/v1/auth/signup") => (202, json!({})),
                ("POST", "/account/v1/auth/login") if !verified => (403, json!({"reason": "unverified"})),
                ("POST", "/account/v1/auth/verify") | ("POST", "/account/v1/auth/login") => {
                    verified = true;
                    rt = 1;
                    (200, json!({"access_token": jwt("acc-1", "dev-1"), "refresh_token": "r1", "expires_in": 900, "key_state": "bootstrap"}))
                }
                ("POST", "/account/v1/auth/refresh") => {
                    if v["refresh_token"] == format!("r{rt}") {
                        rt += 1;
                        (200, json!({"access_token": jwt("acc-1", "dev-1"), "refresh_token": format!("r{rt}"), "expires_in": 900}))
                    } else {
                        (401, json!({"error": "reused"}))
                    }
                }
                ("POST", "/account/v1/keys/grant") => (200, json!({})),
                ("GET", "/account/v1/me") => (200, json!({"email": "a@b.c", "email_verified": true})),
                ("GET", "/account/v1/profiles") => (200, json!({"profiles": [
                    {"profile_id": "p-1", "nickname": "Neto", "icon_id": 3, "tier": 2, "friend_code": "NP-123456789012", "links": [{"kind": "web", "value": "https://x.y"}]},
                    {"profile_id": "p-2", "nickname": "Alt", "icon_id": 1}]})),
                ("GET", "/account/v1/devices") => (200, json!({"devices": [{"device_id": "dev-1", "name": "Mac", "platform": "macos"}]})),
                ("GET", "/account/v1/keys/requests") => (200, json!({"requests": []})),
                ("GET", "/social/v1/friends") => (200, json!({"count": 1, "friends": [{"profile": {"profile_id": "p-9", "nickname": "Pal", "icon_id": 5},
                    "presence": {"status": "playing", "title_id": "0100152000022000", "mode": "online", "joinable": true, "join_token": "tok"}}]})),
                ("GET", "/social/v1/friend-requests") => (200, json!({"incoming": [{"request_id": "rq1", "from": {"profile_id": "p-7", "nickname": "Req", "icon_id": 0}, "to": {"profile_id": "p-1"}}], "outgoing": []})),
                ("POST", "/social/v1/friend-requests/rq1/accept") => (200, json!({"friend": {}})),
                ("POST", "/social/v1/friend-requests") => (201, json!({"status": "pending"})),
                ("GET", "/social/v1/search") => (200, json!({"results": [{"profile_id": "p-5", "nickname": "Found", "icon_id": 2}]})),
                ("PATCH", p) if p.starts_with("/account/v1/profiles/") => (200, json!({"tier": v["tier"]})),
                ("GET", "/social/v1/blocks") | ("GET", "/social/v1/recent") => (200, json!({"blocks": [], "recent": []})),
                ("GET", p) if p.starts_with("/social/v1/profiles/") => (200, json!({"stats": [{"title_id": "0100152000022000", "key": "wins", "value": 4}]})),
                ("GET", "/social/v1/me") => (200, json!({"friend_code": "NP-1234-5678-9012"})),
                _ => (404, json!({"error": "nope"})),
            };
            let _ = req.respond(tiny_http::Response::from_string(out.to_string()).with_status_code(code).with_header("Content-Type: application/json".parse::<tiny_http::Header>().unwrap()));
        }
    });
    (url, log)
}

fn home(n: &str) -> std::path::PathBuf {
    let d = std::env::temp_dir().join(format!("napple-online-{n}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    d
}

fn client(url: &str, h: &std::path::Path) -> Client {
    let mut c = Client::new(h.to_path_buf(), url, url);
    c.creds = CredStore::with_file_key(h);
    c
}

#[test]
fn signup_verify_state_and_friend_actions() {
    let (url, log) = serve();
    let h = home("flow");
    let c = client(&url, &h);
    assert_eq!(c.act("signup", &json!({"email": "a@b.c", "password": "correct horse 1", "nickname": "Neto", "age_ok": true, "terms_ok": true})).0, OK);
    assert_eq!(c.act("signup", &json!({"email": "a@b.c", "password": "x", "nickname": "N", "age_ok": false, "terms_ok": true})).0, ERR_INVALID);
    let (code, j) = c.act("login", &json!({"email": "a@b.c", "password": "pw"}));
    assert_eq!((code, j["reason"].as_str()), (ERR_UNVERIFIED, Some("unverified")));
    assert_eq!(c.act("verify", &json!({"email": "a@b.c", "code": "123456"})).0, OK);

    let s = c.state();
    assert_eq!(s["signed_in"], true);
    assert_eq!(s["email"], "a@b.c");
    assert_eq!(s["current"]["id"], "p-1"); // first profile, picked automatically
    assert_eq!(s["current"]["tier"], 2);
    // the shareable code is the one napple-social accepts (its own NP-dddd-dddd-dddd), not the account profile's
    assert_eq!(s["current"]["friend_code"], "NP-1234-5678-9012");
    assert_eq!(s["current"]["links"][0]["value"], "https://x.y");
    assert_eq!(s["profiles"].as_array().unwrap().len(), 2);
    assert_eq!(s["devices"][0]["current"], true);
    assert_eq!(s["friends"][0]["nickname"], "Pal");
    assert_eq!(s["friends"][0]["online"], true);
    assert_eq!(s["friends"][0]["joinable_token"], "tok");
    assert_eq!(s["requests"][0]["incoming"], true);
    assert_eq!(s["stats"][0]["value"], 4);
    assert_eq!(s["has_account_key"], true);
    // the social calls carried the acting profile
    assert!(log.lock().unwrap().iter().any(|l| l == "GET /social/v1/friends profile=p-1"));
    // no secret in the shared file
    let raw = std::fs::read_to_string(h.join("online/credentials.json")).unwrap();
    assert!(!raw.contains("\"r1\"") && !raw.contains("r1\""));

    assert_eq!(c.act("friend_accept", &json!({"profile_id": "p-7"})).0, OK);
    assert_eq!(c.act("friend_accept", &json!({"profile_id": "nobody"})).0, ERR_INVALID);
    assert_eq!(c.act("friend_request", &json!({"code": "NP-123456789012"})).0, OK);
    let (_, r) = c.act("search", &json!({"query": "Fou nd"}));
    assert_eq!(r["results"][0]["nickname"], "Found");
    let (code, r) = c.act("set_tier", &json!({"profile_id": "p-1", "tier": 4}));
    assert_eq!((code, r["tier"].as_i64()), (OK, Some(4)));
    assert_eq!(c.act("set_tier", &json!({"profile_id": "p-1", "tier": 9})).0, ERR_INVALID);
    // switching the profile is remembered in profile_map.json
    assert_eq!(c.act("switch_profile", &json!({"profile_id": "p-2"})).0, OK);
    assert_eq!(c.state()["current"]["id"], "p-2");
    let c2 = client(&url, &h);
    assert_eq!(c2.profile_id().as_deref(), Some("p-2"));
    let _ = std::fs::remove_dir_all(&h);
}

#[test]
fn two_games_refresh_without_reusing_a_token() {
    let (url, _) = serve();
    let h = home("refresh");
    let a = client(&url, &h);
    assert_eq!(a.act("verify", &json!({"email": "a@b.c", "code": "1"})).0, OK);
    // each "game" is its own Client (own memory) over the same credentials file
    let ts: Vec<_> = (0..4)
        .map(|_| {
            let (u, h) = (url.clone(), h.clone());
            std::thread::spawn(move || {
                let c = client(&u, &h);
                for _ in 0..3 {
                    c.refresh().expect("refresh must never present a rotated token");
                }
            })
        })
        .collect();
    for t in ts {
        t.join().unwrap();
    }
    assert!(a.signed_in());
    assert!(a.access_token().is_some());
    let _ = std::fs::remove_dir_all(&h);
}

#[test]
fn logout_clears_and_unreachable_is_a_network_error() {
    let (url, _) = serve();
    let h = home("logout");
    let c = client(&url, &h);
    assert_eq!(c.act("verify", &json!({"email": "a@b.c", "code": "1"})).0, OK);
    assert_eq!(c.act("logout", &json!({})).0, OK);
    assert!(!c.signed_in());
    assert_eq!(c.state()["signed_in"], false);
    let dead = client("http://127.0.0.1:1", &h);
    assert_eq!(dead.act("login", &json!({"email": "a@b.c", "password": "pw"})).0, ERR_NETWORK);
    let _ = std::fs::remove_dir_all(&h);
}
