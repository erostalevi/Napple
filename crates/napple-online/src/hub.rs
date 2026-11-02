//! The identity hub: napple-online's link to the Napple Account (who is this player) and napple-social (who did they meet), switched on by
//! `NAPPLE_ACCOUNT_URL`. Without it the server keeps its legacy self-issued accounts and tokens (LAN / dev use) and none of this runs.
//!
//! Login: the NEX token is the account access JWT; the NEX login name may carry the acting profile and the title:
//! `<profile_uuid>` or `<profile_uuid>/<title_id16>` (anything else: the account's first profile, title "unknown").
//! The game-visible principal is `identity::principal_id(profile_id)`. The name the game shows is the nickname, or at tier 5 a ghost alias.
//! Env: `NAPPLE_ACCOUNT_URL` (http://, the local account listener), `NAPPLE_SOCIAL_URL` + `NAPPLE_SOCIAL_SERVICE_TOKEN` (reporting; both
//! needed), `NAPPLE_ONLINE_ADMIN_ACCOUNTS` (comma-separated account UUIDs allowed to call the admin endpoints).

use crate::accounts::now_secs;
use crate::ghost::Ghosts;
use crate::http::HttpRequest;
use crate::identity::{is_uuid, principal_id, AuthError, Verifier};
use crate::matchlog::{Match, MatchLog, Who};
use crate::reporting::Reporter;
use crate::upstream::{HttpUpstream, Upstream};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};

#[derive(Clone)]
struct Principal {
    who: Who,
    title_id: String,
}

pub struct Hub {
    verifier: Verifier,
    ghosts: Ghosts,
    pub log: MatchLog,
    reporter: Option<Arc<Reporter>>,
    admins: Vec<String>,
    principals: Mutex<HashMap<u64, Principal>>,
}

fn env(name: &str) -> Option<String> {
    std::env::var(name).ok().filter(|v| !v.is_empty())
}

impl Hub {
    pub fn new(up: Arc<dyn Upstream>, account_url: &str, social: Option<(&str, &str)>, admins: Vec<String>, data: Option<PathBuf>) -> Hub {
        Hub {
            verifier: Verifier::new(up.clone(), account_url),
            ghosts: Ghosts::default(),
            log: MatchLog::open(data),
            reporter: social.map(|(url, tok)| Arc::new(Reporter::new(up, url, tok))),
            admins,
            principals: Mutex::new(HashMap::new()),
        }
    }

    /// Built from the environment; None when `NAPPLE_ACCOUNT_URL` is unset (legacy mode).
    pub fn from_env(data: PathBuf) -> Option<Arc<Hub>> {
        let account = env("NAPPLE_ACCOUNT_URL")?;
        let social = env("NAPPLE_SOCIAL_URL").zip(env("NAPPLE_SOCIAL_SERVICE_TOKEN"));
        let admins = env("NAPPLE_ONLINE_ADMIN_ACCOUNTS").map(|v| v.split(',').map(|s| s.trim().to_ascii_lowercase()).filter(|s| !s.is_empty()).collect()).unwrap_or_default();
        if social.is_none() {
            println!("napple-online: NAPPLE_SOCIAL_URL / NAPPLE_SOCIAL_SERVICE_TOKEN not both set: encounters and stats are not reported");
        }
        Some(Arc::new(Hub::new(Arc::new(HttpUpstream), &account, social.as_ref().map(|(a, b)| (a.as_str(), b.as_str())), admins, Some(data))))
    }

    /// Verifies `token` (NUL padding allowed) and registers the principal. `user` is the NEX login name (see the module doc).
    pub fn login(&self, token: &str, user: &str, now: u64) -> Result<u64, AuthError> {
        let claims = self.verifier.verify(token, now)?;
        let user = user.trim_end_matches('\0');
        let (first, title) = user.split_once('/').unwrap_or((user, ""));
        let want = is_uuid(first).then_some(first);
        let title = if title.len() == 16 && title.bytes().all(|c| c.is_ascii_hexdigit()) { title.to_ascii_lowercase() } else { "unknown".into() };
        let profile = self.verifier.profile(token, &claims, want, now)?;
        let pid = principal_id(&profile.profile_id);
        let shown_as = if profile.tier >= 5 {
            let me = profile.nickname.clone();
            self.ghosts.alias(pid, &|n| n.eq_ignore_ascii_case(&me) || self.principals.lock().unwrap().values().any(|p| p.who.tier < 5 && p.who.nickname.eq_ignore_ascii_case(n)))
        } else {
            profile.nickname.clone()
        };
        let who = Who { account_id: claims.account_id, profile_id: profile.profile_id, pid, tier: profile.tier, nickname: profile.nickname, shown_as };
        self.principals.lock().unwrap().insert(pid, Principal { who, title_id: title });
        Ok(pid)
    }

    pub fn known(&self, pid: u64) -> bool {
        self.principals.lock().unwrap().contains_key(&pid)
    }

    /// The name the game shows for `pid`: nickname, or the ghost alias.
    pub fn display_name(&self, pid: u64) -> Option<String> {
        self.principals.lock().unwrap().get(&pid).map(|p| p.who.shown_as.clone())
    }

    pub fn on_join(&self, gid: u32, pid: u64) {
        let p = self.principals.lock().unwrap().get(&pid).cloned();
        if let Some(p) = p {
            self.log.join(gid, p.who, &p.title_id, now_secs());
        }
    }

    /// `ended`: the gathering is gone (nobody left): the match is closed, stored and reported to napple-social (in the background).
    pub fn on_leave(&self, gid: u32, pid: u64, ended: bool) {
        let now = now_secs();
        self.log.leave(gid, pid, now);
        if ended {
            if let Some(m) = self.log.close(gid, now) {
                self.report(m);
            }
        }
    }

    fn report(&self, m: Match) {
        if let Some(r) = &self.reporter {
            r.report_match(&m);
            let r = r.clone();
            std::thread::spawn(move || {
                r.flush();
            });
        }
    }

    /// The player's last connection ended: its ghost alias is released and the next session gets a new one.
    pub fn logout(&self, pid: u64) {
        self.principals.lock().unwrap().remove(&pid);
        self.ghosts.release(pid);
    }

    /// Hourly job: purges the match log past its 30 days and retries unsent reports.
    pub fn spawn_maintenance(hub: Arc<Hub>) {
        std::thread::spawn(move || loop {
            let n = hub.log.purge(now_secs());
            if n > 0 {
                println!("napple-online: purged {n} matches older than 30 days from the match log");
            }
            if let Some(r) = &hub.reporter {
                r.flush();
            }
            std::thread::sleep(std::time::Duration::from_secs(3600));
        });
    }

    fn is_admin(&self, req: &HttpRequest, now: u64) -> bool {
        let Some(tok) = req.header("authorization").and_then(|v| v.strip_prefix("Bearer ")) else { return false };
        self.verifier.verify(tok, now).map(|c| self.admins.contains(&c.account_id)).unwrap_or(false)
    }

    /// The hub's HTTP surface. None = not ours (falls through to the legacy API, which is then switched off by the caller).
    ///  * `POST /online/reports {alias, match_id?}` (admin): who is behind the alias a player reported.
    ///  * `GET /online/matchlog/export?since=<unix>` (admin).
    ///  * legacy `/napple/v1/{account,token,friends}`: 410, accounts live in napple-account now.
    pub fn http(&self, req: &HttpRequest, now: u64) -> Option<(u16, &'static str, String)> {
        let path = req.path.split('?').next().unwrap_or("");
        match (req.method.as_str(), path) {
            ("POST", "/online/reports") | ("GET", "/online/matchlog/export") => {
                if !self.is_admin(req, now) {
                    return Some((403, "Forbidden", json!({"error": "admin only"}).to_string()));
                }
                if path == "/online/matchlog/export" {
                    let since = req.path.split_once("since=").and_then(|(_, v)| v.split('&').next()).and_then(|v| v.parse().ok()).unwrap_or(0);
                    return Some((200, "OK", self.log.export(since).to_string()));
                }
                let body: Value = serde_json::from_slice(&req.body).unwrap_or(Value::Null);
                let Some(alias) = body["alias"].as_str() else { return Some((400, "Bad Request", json!({"error": "alias required"}).to_string())) };
                let found: Vec<Value> = self.log.resolve(alias, body["match_id"].as_str()).into_iter().map(|(m, e)| json!({"match_id": m.match_id, "title_id": m.title_id, "started": m.started, "ended": m.ended, "account_id": e.who.account_id, "profile_id": e.who.profile_id, "nickname": e.who.nickname, "tier": e.who.tier, "shown_as": e.who.shown_as, "joined": e.joined, "left": e.left, "participants": m.participants.iter().map(|p| p.who.profile_id.clone()).collect::<Vec<_>>()})).collect();
                Some((if found.is_empty() { 404 } else { 200 }, if found.is_empty() { "Not Found" } else { "OK" }, json!({"matches": found}).to_string()))
            }
            (_, "/napple/v1/account" | "/napple/v1/token" | "/napple/v1/friends") => Some((410, "Gone", "error=accounts are issued by napple-account; sign in there\n".into())),
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::identity::testutil::*;
    use serde_json::json;
    use std::sync::atomic::Ordering;

    const NOW: u64 = 1_800_000_000;
    const PROF2: &str = "0192f2a0-0000-7000-8000-0000000000a2";
    const ADMIN: &str = "0192f2a0-0000-7000-8000-0000000000ad";

    fn token(k: &ed25519_dalek::SigningKey, sub: &str) -> String {
        let n = now_secs();
        let mut c = good_claims(n);
        c["sub"] = json!(sub);
        mint(k, "k1", c)
    }

    fn hub() -> (Arc<Mock>, Hub, ed25519_dalek::SigningKey) {
        let k = key(7);
        let m = Mock::with_key("k1", &k);
        m.profiles.lock().unwrap().push((PROF2.into(), "Luigi_".into(), 5));
        (m.clone(), Hub::new(m, "http://acct", Some(("http://social", "svc")), vec![ADMIN.into()], None), k)
    }

    #[test]
    fn login_derives_pid_from_profile_and_names() {
        let (_m, h, k) = hub();
        let t = token(&k, ACC);
        let p1 = h.login(&t, "", now_secs()).unwrap();
        assert_eq!(p1, principal_id(PROF));
        assert_eq!(h.display_name(p1).unwrap(), "Mario_");
        // a named profile and title; the same profile gives the same pid on any device
        let p2 = h.login(&t, &format!("{PROF2}/0100152000022000\0"), now_secs()).unwrap();
        assert_eq!(p2, principal_id(PROF2));
        let alias = h.display_name(p2).unwrap();
        assert_ne!(alias, "Luigi_", "tier 5 never shows the real nickname");
        assert_eq!(h.login(&t, PROF2, now_secs()).unwrap(), p2);
        assert_eq!(h.display_name(p2).unwrap(), alias, "alias is stable within a session");
        h.logout(p2);
        assert!(!h.known(p2));
        // not the caller's profile / forged token / expired token
        assert_eq!(h.login(&t, "0192f2a0-0000-7000-8000-0000000000ff", now_secs()), Err(AuthError::NoProfile));
        assert!(h.login("eyJhbGciOiJFZERTQSIsImtpZCI6ImsxIn0.e30.AAAA", "", now_secs()).is_err());
        let mut old = good_claims(1000);
        old["exp"] = json!(1900);
        assert_eq!(h.login(&mint(&k, "k1", old), "", now_secs()), Err(AuthError::Expired));
    }

    #[test]
    fn match_lifecycle_logs_real_accounts_and_reports_without_ghosts() {
        let (m, h, k) = hub();
        let t = token(&k, ACC);
        let title = "0100152000022000";
        let a = h.login(&t, &format!("{PROF}/{title}"), now_secs()).unwrap();
        let g = h.login(&t, &format!("{PROF2}/{title}"), now_secs()).unwrap();
        h.on_join(7, a);
        h.on_join(7, g);
        let alias = h.display_name(g).unwrap();
        let id = h.log.match_id_of(7).unwrap();
        // a report of the alias resolves to the real account for an admin
        let adm = token(&k, ADMIN);
        let mk = |method: &str, path: &str, tok: &str, body: Value| HttpRequest { method: method.into(), path: path.into(), headers: [("authorization".to_string(), format!("Bearer {tok}"))].into(), body: body.to_string().into_bytes() };
        let (st, _, body) = h.http(&mk("POST", "/online/reports", &adm, json!({"alias": alias, "match_id": id})), now_secs()).unwrap();
        let v: Value = serde_json::from_str(&body).unwrap();
        assert_eq!(st, 200);
        assert_eq!((v["matches"][0]["account_id"].as_str(), v["matches"][0]["profile_id"].as_str(), v["matches"][0]["tier"].as_u64()), (Some(ACC), Some(PROF2), Some(5)));
        // not for ordinary accounts
        assert_eq!(h.http(&mk("POST", "/online/reports", &t, json!({"alias": alias})), now_secs()).unwrap().0, 403);
        assert_eq!(h.http(&mk("GET", "/online/matchlog/export?since=0", &adm, Value::Null), now_secs()).unwrap().0, 200);
        h.on_leave(7, a, false);
        h.on_leave(7, g, true);
        for _ in 0..200 {
            if m.posts.lock().unwrap().len() >= 3 {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        let posts = m.posts.lock().unwrap();
        assert_eq!(posts.iter().filter(|p| p.0.ends_with("/stats")).count(), 1, "stats only for the non-ghost");
        assert!(!posts.iter().any(|p| p.3.to_string().contains(PROF2)), "no trace of the ghost");
        assert_eq!(h.log.resolve(&alias, None).len(), 1, "the log keeps the ghost's real identity");
        let _ = m.jwks_calls.load(Ordering::SeqCst);
    }

    #[test]
    fn legacy_endpoints_are_gone() {
        let (_m, h, _k) = hub();
        let r = HttpRequest { method: "POST".into(), path: "/napple/v1/token".into(), headers: Default::default(), body: vec![] };
        assert_eq!(h.http(&r, NOW).unwrap().0, 410);
        let r = HttpRequest { method: "GET".into(), path: "/healthz".into(), headers: Default::default(), body: vec![] };
        assert!(h.http(&r, NOW).is_none());
    }
}
