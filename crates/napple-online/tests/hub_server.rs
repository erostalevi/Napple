//! End to end through the public server API with a mock napple-account and napple-social: an account JWT logs in (the pid is the profile's),
//! a ghost is shown by alias, the match is logged with the real accounts, and when it ends encounters and stats go to social (ghost excluded).

use ed25519_dalek::{Signer, SigningKey};
use napple_online::accounts::{now_secs, Accounts};
use napple_online::crypto::base64;
use napple_online::hub::Hub;
use napple_online::identity::principal_id;
use napple_online::matchmaking::{Gathering, MatchmakeSession};
use napple_online::nex::{Reader, Writer};
use napple_online::rmc::Request;
use napple_online::server::*;
use serde_json::{json, Value};
use std::sync::mpsc::channel;
use std::sync::{Arc, Mutex};

const TITLE: &str = "0100152000022000";
const P1: &str = "0192f2a0-0000-7000-8000-0000000000a1";
const P2: &str = "0192f2a0-0000-7000-8000-0000000000a2";

struct Mock {
    key: SigningKey,
    posts: Mutex<Vec<(String, Value)>>,
}
impl napple_online::upstream::Upstream for Mock {
    fn request(&self, _m: &str, url: &str, bearer: Option<&str>, _h: &[(&str, &str)], body: Option<&str>) -> Result<(u16, Vec<u8>), String> {
        if url.ends_with("/jwks.json") {
            return Ok((200, json!({"keys": [{"kty":"OKP","crv":"Ed25519","kid":"k","x": base64(&self.key.verifying_key().to_bytes(), true)}]}).to_string().into_bytes()));
        }
        if url.ends_with("/account/v1/profiles") {
            assert!(bearer.is_some());
            return Ok((200, json!({"profiles": [{"profile_id": P1, "nickname": "Mario_", "tier": 1}, {"profile_id": P2, "nickname": "Luigi_", "tier": 5}]}).to_string().into_bytes()));
        }
        self.posts.lock().unwrap().push((url.into(), serde_json::from_str(body.unwrap()).unwrap()));
        Ok((201, b"{}".to_vec()))
    }
}

fn jwt(k: &SigningKey, sub: &str) -> String {
    let h = base64(br#"{"alg":"EdDSA","kid":"k"}"#, true);
    let p = base64(json!({"sub": sub, "did": "d", "aud": "napple", "exp": now_secs() + 900}).to_string().as_bytes(), true);
    format!("{h}.{p}.{}", base64(&k.sign(format!("{h}.{p}").as_bytes()).to_bytes(), true))
}

fn login(srv: &Server, token: &str, user: &str) -> Result<u64, u32> {
    let mut info = Writer::new();
    info.structure(0, |_| {});
    info.structure(0, |w| {
        w.string(token).u32(4).u8(2).u32(0);
    });
    let mut b = Writer::new();
    b.string(user).any("AuthenticationInfo", &info.buf);
    let mut st = AuthState::default();
    match srv.auth_call(&mut st, &Request { protocol: PROTO_AUTH, call_id: 1, method: 2, body: b.buf }) {
        Reply::Ok(r) => {
            let mut r = Reader::new(&r);
            r.u32().unwrap();
            Ok(r.pid().unwrap())
        }
        Reply::Err(e) => Err(e),
    }
}

fn name_of(srv: &Server, pid: u64) -> String {
    let mut b = Writer::new();
    b.pid(pid);
    let Reply::Ok(r) = srv.auth_call(&mut AuthState::default(), &Request { protocol: PROTO_AUTH, call_id: 1, method: 5, body: b.buf }) else { panic!() };
    Reader::new(&r).string().unwrap()
}

fn matchmake(srv: &Server, me: Caller) {
    let template = MatchmakeSession { g: Gathering { max: 12, min: 2, ..Default::default() }, game_mode: 3, open: true, system_type: 2, ..Default::default() };
    let mut body = Writer::new();
    body.list(&[0], |w, _| {
        w.structure(0, |w| {
            w.list(&[""], |w, s: &&str| {
                w.string(s);
            });
            w.string("3").string("").string("").string("").bool(true).bool(false).bool(false).u32(0);
        });
    });
    body.any("MatchmakeSession", &template.to_bytes()).string("hi");
    assert!(matches!(srv.secure_call(me, &Request { protocol: PROTO_MATCHMAKE_EXT, call_id: 1, method: 15, body: body.buf }), Reply::Ok(_)));
}

#[test]
fn account_jwt_login_ghost_matchlog_and_reporting() {
    let up = Arc::new(Mock { key: SigningKey::from_bytes(&[3; 32]), posts: Mutex::new(vec![]) });
    let hub = Arc::new(Hub::new(up.clone(), "http://acct", Some(("http://social", "svc")), vec![], None));
    let srv = Server::new(Accounts::open(None).unwrap(), false).with_hub(hub.clone());
    let tok = jwt(&up.key, "0192f2a0-0000-7000-8000-000000000001");
    let tok2 = jwt(&up.key, "0192f2a0-0000-7000-8000-000000000002");

    // the legacy token of this server no longer logs in
    let (a, _) = srv.accounts.create("4e6170706c6555736572000000000001").unwrap();
    assert!(login(&srv, &srv.accounts.issue_token(a.pid, TITLE, now_secs()), "").is_err());
    assert!(login(&srv, "garbage", "").is_err());

    let p1 = login(&srv, &tok, &format!("{P1}/{TITLE}")).unwrap();
    let p2 = login(&srv, &tok2, &format!("{P2}/{TITLE}")).unwrap();
    assert_eq!((p1, p2), (principal_id(P1), principal_id(P2)));
    assert_eq!(name_of(&srv, p1), "Mario_");
    let alias = name_of(&srv, p2);
    assert_ne!(alias, "Luigi_");

    let (tx1, _r1) = channel();
    let (tx2, _r2) = channel();
    let c1 = Caller { pid: p1, cid: srv.attach(p1, "10.0.0.1".parse().unwrap(), tx1) };
    let c2 = Caller { pid: p2, cid: srv.attach(p2, "10.0.0.2".parse().unwrap(), tx2) };
    matchmake(&srv, c1);
    matchmake(&srv, c2);
    let logged = hub.log.resolve(&alias, None);
    assert_eq!(logged.len(), 1, "both joined one match; the alias maps to the real profile");
    assert_eq!((logged[0].1.who.profile_id.as_str(), logged[0].0.title_id.as_str(), logged[0].0.participants.len()), (P2, TITLE, 2));

    srv.detach(c1.cid);
    srv.detach(c2.cid);
    for _ in 0..200 {
        if !up.posts.lock().unwrap().is_empty() {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(10));
    }
    std::thread::sleep(std::time::Duration::from_millis(50));
    let posts = up.posts.lock().unwrap();
    assert!(posts.iter().all(|(_, b)| !b.to_string().contains(P2)), "the ghost leaves no trace in social");
    assert_eq!(posts.len(), 1, "one stats post for the single visible player; no encounter with only one");
    assert_eq!(posts[0].0, "http://social/social/v1/internal/stats");
    // the next session gets a new alias, and the closed match stays resolvable
    assert!(!hub.known(p2));
    assert_eq!(hub.log.resolve(&alias, None).len(), 1);
}
