//! The game server's shared state and its NEX protocols: Authentication (10) on the authentication port, and on the secure port Secure
//! Connection (11), Matchmake Extension (109), Match Making (21), Match Making Ext (50), NAT Traversal (3), Health (18) and Notification (14,
//! server to client). Ranking, DataStore, Utility and anything else answer Core::NotImplemented, which titles treat as "nothing there".

use crate::accounts::{now_secs, Accounts};
use crate::crypto::{hex, random_bytes};
use crate::kerberos;
use crate::matchmaking::{Criteria, MatchmakeSession, Participant, Sessions, GATHERING_FLAG_VERBOSE_PARTICIPANTS};
use crate::nex::{DateTime, Reader, StationUrl, Writer};
use crate::rmc::{self, err, Request};
use std::collections::HashMap;
use std::net::IpAddr;
use std::sync::mpsc::Sender;
use std::sync::Mutex;

pub const PROTO_NAT_TRAVERSAL: u16 = 3;
pub const PROTO_AUTH: u16 = 10;
pub const PROTO_SECURE: u16 = 11;
pub const PROTO_NOTIFICATION: u16 = 14;
pub const PROTO_HEALTH: u16 = 18;
pub const PROTO_MATCH_MAKING: u16 = 21;
pub const PROTO_MATCH_MAKING_EXT: u16 = 50;
pub const PROTO_MATCHMAKE_EXT: u16 = 109;
pub const PROTO_UTILITY: u16 = 110;
pub const PROTO_RANKING: u16 = 112;

/// NEX "success" as a %retval% Result.
pub const RESULT_SUCCESS: u32 = 0x0001_0001;
/// The principal id of the secure server (what tickets are issued for and the station URL names).
pub const SECURE_PID: u64 = 2;
/// The PRUDP ports of the two servers inside one WebSocket.
pub const PORT_AUTH: u8 = 1;
pub const PORT_SECURE: u8 = 2;

/// Notification types (NotificationEvent.m_uiType).
pub mod notif {
    pub const PARTICIPANT_JOINED: u32 = 3001;
    pub const PARTICIPANT_DISCONNECTED: u32 = 3007;
    pub const PARTICIPATION_ENDED: u32 = 3008;
    pub const OWNERSHIP_CHANGED: u32 = 4000;
    pub const GATHERING_UNREGISTERED: u32 = 109000;
}

pub enum Reply {
    Ok(Vec<u8>),
    Err(u32),
}

pub struct Client {
    pub pid: u64,
    pub cid: u32,
    pub peer: IpAddr,
    pub urls: Vec<StationUrl>,
    /// RMC messages (server requests) to deliver on this client's secure connection.
    pub tx: Sender<Vec<u8>>,
}

pub struct World {
    next_cid: u32,
    next_call: u32,
    pub clients: HashMap<u32, Client>,
    pub sessions: Sessions,
    /// Ranking common data (the player's Mii and name blob) per principal.
    pub common_data: HashMap<u64, Vec<u8>>,
}

/// What an authentication connection remembers between calls.
#[derive(Default)]
pub struct AuthState {
    pub pid: Option<u64>,
    pub source_key: Vec<u8>,
}

/// Who is calling on the secure port.
#[derive(Clone, Copy)]
pub struct Caller {
    pub pid: u64,
    pub cid: u32,
}

pub struct Server {
    pub accounts: Accounts,
    pub secure_key: [u8; 16],
    pub world: Mutex<World>,
    pub verbose: bool,
}

fn ok(f: impl FnOnce(&mut Writer)) -> Reply {
    let mut w = Writer::new();
    f(&mut w);
    Reply::Ok(w.buf)
}

/// The secure server's station URL as Switch titles expect it: same server (0.0.0.1, port 1), PRUDP port PORT_SECURE.
pub fn secure_station_url() -> String {
    format!("prudps:/address=0.0.0.1;port=1;CID=1;PID={SECURE_PID};sid={PORT_SECURE};stream=10;type=2")
}

/// The token inside an AuthenticationInfo / NintendoLoginData (Data parent, then the structure whose first field is the token).
pub fn token_from_extra(object: &[u8]) -> Option<String> {
    let parsed = (|| -> crate::nex::Result<String> {
        let mut r = Reader::new(object);
        r.structure()?; // Data
        let (_v, mut a) = r.structure()?;
        a.string()
    })();
    if let Ok(t) = parsed {
        if !t.is_empty() {
            return Some(t);
        }
    }
    // fall back to the first JWS-looking run of characters
    let s = String::from_utf8_lossy(object);
    let at = s.find("eyJ")?;
    let tok: String = s[at..].chars().take_while(|c| c.is_ascii_alphanumeric() || matches!(c, '-' | '_' | '.')).collect();
    Some(tok)
}

impl Server {
    pub fn new(accounts: Accounts, verbose: bool) -> Server {
        let secure_key = kerberos::secure_key(accounts.server_key());
        Server { accounts, secure_key, world: Mutex::new(World { next_cid: 1, next_call: 1, clients: HashMap::new(), sessions: Sessions::new(), common_data: HashMap::new() }), verbose }
    }

    /// A secure connection was accepted: the client gets a connection id.
    pub fn attach(&self, pid: u64, peer: IpAddr, tx: Sender<Vec<u8>>) -> u32 {
        let mut w = self.world.lock().unwrap();
        let cid = w.next_cid;
        w.next_cid += 1;
        w.clients.insert(cid, Client { pid, cid, peer, urls: vec![], tx });
        cid
    }

    /// The secure connection ended: the client leaves every session it was in.
    pub fn detach(&self, cid: u32) {
        let mut w = self.world.lock().unwrap();
        let Some(c) = w.clients.remove(&cid) else { return };
        for gid in w.sessions.of(c.pid) {
            self.leave(&mut w, gid, c.pid, notif::PARTICIPANT_DISCONNECTED, "");
        }
    }

    fn send_request(&self, w: &mut World, cid: u32, protocol: u16, method: u32, body: &[u8]) {
        let call = w.next_call;
        w.next_call = w.next_call.wrapping_add(1);
        if let Some(c) = w.clients.get(&cid) {
            let _ = c.tx.send(rmc::encode_request(protocol, call, method, body));
        }
    }
    fn notify(&self, w: &mut World, to_pid: u64, source: u64, kind: u32, p1: u64, p2: u64, s: &str, p3: u64) {
        let mut b = Writer::new();
        b.structure(0, |b| {
            b.pid(source).u32(kind).u64(p1).u64(p2).string(s).u64(p3);
        });
        let targets: Vec<u32> = w.clients.values().filter(|c| c.pid == to_pid).map(|c| c.cid).collect();
        for cid in targets {
            self.send_request(w, cid, PROTO_NOTIFICATION, 1, &b.buf);
        }
    }

    fn leave(&self, w: &mut World, gid: u32, pid: u64, kind: u32, msg: &str) -> bool {
        let old_owner = w.sessions.all.get(&gid).map(|s| s.mm.g.owner);
        let Some(left) = w.sessions.leave(gid, pid) else { return false };
        if left.ended {
            return true;
        }
        let owner = w.sessions.all[&gid].mm.g.owner;
        if old_owner != Some(pid) {
            self.notify(w, owner, pid, kind, gid as u64, pid, msg, 0);
        }
        if let Some(n) = left.new_owner {
            for p in &left.remaining {
                self.notify(w, p.pid, pid, notif::OWNERSHIP_CHANGED, gid as u64, n, &format!("{}", now_secs() * 1_000_000), 0);
            }
        }
        true
    }

    // ---------------- Authentication (10) ----------------

    fn login(&self, st: &mut AuthState, user: &str, extra: &[u8]) -> Result<(u64, Vec<u8>, String), u32> {
        let token = token_from_extra(extra).ok_or(err::RV_INVALID_USERNAME)?;
        let claims = self.accounts.verify_token(&token, now_secs()).ok_or(err::RV_ACCOUNT_EXPIRED)?;
        if let Ok(n) = user.trim_end_matches('\0').parse::<u64>() {
            if n != claims.pid {
                println!("napple-online: login name {n} differs from the token's account {}; the token wins", claims.pid);
            }
        }
        let source_key = random_bytes(16);
        let session = random_bytes(kerberos::SESSION_KEY_LEN);
        let internal = kerberos::internal_ticket(&self.secure_key, now_secs(), claims.pid, &session);
        let ticket = kerberos::client_ticket(&source_key, &session, SECURE_PID, &internal);
        st.pid = Some(claims.pid);
        st.source_key = source_key.clone();
        Ok((claims.pid, ticket, hex(&source_key)))
    }

    fn rv_connection_data(w: &mut Writer) {
        w.structure(1, |w| {
            w.string(&secure_station_url()).u32(0).string("prudp:/").datetime(DateTime::now());
        });
    }

    pub fn auth_call(&self, st: &mut AuthState, req: &Request) -> Reply {
        let mut r = Reader::new(&req.body);
        let return_msg = format!("napple-online {}", env!("CARGO_PKG_VERSION"));
        match (req.protocol, req.method) {
            (PROTO_AUTH, 1) => Reply::Err(err::RV_INVALID_USERNAME), // name + password logins do not exist here: a Napple token is required
            (PROTO_AUTH, 2) => {
                let (Ok(user), Ok((_, extra))) = (r.string(), r.any()) else { return Reply::Err(err::CORE_INVALID_ARGUMENT) };
                match self.login(st, &user, extra) {
                    Ok((pid, ticket, source_key)) => ok(|w| {
                        w.result(RESULT_SUCCESS).pid(pid).buffer(&ticket);
                        Server::rv_connection_data(w);
                        w.string(&return_msg).string(&source_key);
                    }),
                    Err(e) => Reply::Err(e),
                }
            }
            (PROTO_AUTH, 6) => {
                let parsed = (|| -> crate::nex::Result<(String, Vec<u8>)> {
                    let (_v, mut p) = r.structure()?;
                    p.u32()?; // platform
                    let user = p.string()?;
                    let (_, extra) = p.any()?;
                    Ok((user, extra.to_vec()))
                })();
                let Ok((user, extra)) = parsed else { return Reply::Err(err::CORE_INVALID_ARGUMENT) };
                match self.login(st, &user, &extra) {
                    Ok((pid, ticket, source_key)) => ok(|w| {
                        w.structure(0, |w| {
                            w.pid(pid).buffer(&ticket).string(&secure_station_url()).datetime(DateTime::now()).string(&return_msg).string(&source_key);
                        });
                    }),
                    Err(e) => Reply::Err(e),
                }
            }
            (PROTO_AUTH, 3) => {
                let (Ok(src), Ok(target)) = (r.pid(), r.pid()) else { return Reply::Err(err::CORE_INVALID_ARGUMENT) };
                if st.pid != Some(src) || target != SECURE_PID {
                    return Reply::Err(err::CORE_ACCESS_DENIED);
                }
                let session = random_bytes(kerberos::SESSION_KEY_LEN);
                let internal = kerberos::internal_ticket(&self.secure_key, now_secs(), src, &session);
                let ticket = kerberos::client_ticket(&st.source_key, &session, SECURE_PID, &internal);
                let sk = hex(&st.source_key);
                ok(|w| {
                    w.result(RESULT_SUCCESS).buffer(&ticket).string(&sk);
                })
            }
            (PROTO_AUTH, 4) => {
                let pid = r.string().ok().and_then(|n| n.parse::<u64>().ok()).filter(|p| self.accounts.get(*p).is_some()).unwrap_or(0);
                ok(|w| {
                    w.pid(pid);
                })
            }
            (PROTO_AUTH, 5) => {
                let name = r.pid().ok().filter(|p| self.accounts.get(*p).is_some()).map(|p| p.to_string()).unwrap_or_default();
                ok(|w| {
                    w.string(&name);
                })
            }
            _ => Reply::Err(err::CORE_NOT_IMPLEMENTED),
        }
    }

    // ---------------- secure port ----------------

    pub fn secure_call(&self, me: Caller, req: &Request) -> Reply {
        match req.protocol {
            PROTO_SECURE => self.secure(me, req),
            PROTO_MATCHMAKE_EXT => self.matchmake_ext(me, req),
            PROTO_MATCH_MAKING => self.match_making(me, req),
            PROTO_MATCH_MAKING_EXT => self.match_making_ext(me, req),
            PROTO_NAT_TRAVERSAL => self.nat_traversal(me, req),
            PROTO_UTILITY => self.utility(me, req),
            PROTO_RANKING => self.ranking(me, req),
            PROTO_HEALTH => ok(|w| {
                w.bool(true);
            }),
            _ => Reply::Err(err::CORE_NOT_IMPLEMENTED),
        }
    }

    fn secure(&self, me: Caller, req: &Request) -> Reply {
        let mut r = Reader::new(&req.body);
        let mut w = self.world.lock().unwrap();
        match req.method {
            1 | 4 => {
                let Ok(urls) = r.list(|r| r.string()) else { return Reply::Err(err::CORE_INVALID_ARGUMENT) };
                let Some(c) = w.clients.get_mut(&me.cid) else { return Reply::Err(err::RV_SESSION_VOID) };
                c.urls = urls.iter().map(|u| StationUrl::parse(u)).collect();
                for u in c.urls.iter_mut() {
                    u.set("RVCID", me.cid);
                    u.set("PID", me.pid);
                }
                // what the server sees: the same station at the address the connection comes from
                let mut public = c.urls.first().cloned().unwrap_or_else(|| StationUrl::parse("prudp:/"));
                public.set("address", crate::natcheck::observed_ip(c.peer));
                let public = public.to_string();
                let cid = me.cid;
                ok(|w| {
                    w.result(RESULT_SUCCESS).u32(cid).string(&public);
                })
            }
            2 | 3 => {
                let (Ok(cid), Ok(pid)) = (r.u32(), r.pid()) else { return Reply::Err(err::CORE_INVALID_ARGUMENT) };
                let found: Vec<(String, u32)> = w
                    .clients
                    .values()
                    .filter(|c| c.cid == cid || c.pid == pid)
                    .flat_map(|c| c.urls.iter().map(move |u| (u.to_string(), c.cid)))
                    .collect();
                let method = req.method;
                ok(|w| {
                    w.bool(!found.is_empty());
                    if method == 2 {
                        w.list(&found, |w, (u, c)| {
                            w.structure(0, |w| {
                                w.string(u).u32(*c);
                            });
                        });
                    } else {
                        w.list(&found, |w, (u, _)| {
                            w.string(u);
                        });
                    }
                })
            }
            6 => {
                if let (Ok(urls), Some(c)) = (r.list(|r| r.string()), w.clients.get_mut(&me.cid)) {
                    c.urls = urls.iter().map(|u| StationUrl::parse(u)).collect();
                }
                ok(|_| {})
            }
            7 => {
                if let (Ok(old), Ok(new), Some(c)) = (r.string(), r.string(), w.clients.get_mut(&me.cid)) {
                    let old = StationUrl::parse(&old);
                    for u in c.urls.iter_mut() {
                        if u.get("address") == old.get("address") && u.get("port") == old.get("port") {
                            *u = StationUrl::parse(&new);
                        }
                    }
                }
                ok(|_| {})
            }
            5 | 8 => ok(|_| {}),
            _ => Reply::Err(err::CORE_NOT_IMPLEMENTED),
        }
    }

    // ---------------- matchmaking ----------------

    /// "New participant" (3001) for `joiner` in `gid`. A gathering with a verbose-participants flag (0x200 / 0x800, MK8D sets both) tells every
    /// participant, the joiner included, even when it just created the gathering: MK8D waits for its own join event before it starts its
    /// session (10 s, then SessionKeepFailed 2618-0562). Otherwise only the owner hears about others joining.
    fn announce_join(&self, w: &mut World, gid: u32, joiner: u64, msg: &str) {
        let Some(s) = w.sessions.all.get(&gid) else { return };
        let (owner, n, flags) = (s.mm.g.owner, s.players(), s.mm.g.flags);
        let targets: Vec<u64> = if flags & GATHERING_FLAG_VERBOSE_PARTICIPANTS != 0 {
            let mut v: Vec<u64> = s.participants.iter().map(|p| p.pid).collect();
            v.dedup();
            v
        } else if owner != joiner {
            vec![owner]
        } else {
            vec![]
        };
        for t in targets {
            self.notify(w, t, joiner, notif::PARTICIPANT_JOINED, gid as u64, joiner, msg, n as u64);
        }
    }
    fn join_existing(&self, w: &mut World, me: Caller, gid: u32, count: u16, msg: &str) -> Result<(), u32> {
        w.sessions.join(gid, me.pid, me.cid, count).map_err(|_| err::RV_SESSION_VOID)?;
        self.announce_join(w, gid, me.pid, msg);
        Ok(())
    }
    /// Find-or-create: the matchmaking every "_Postpone" call does.
    fn auto_matchmake(&self, me: Caller, template: MatchmakeSession, criteria: &[Criteria], count: u16, msg: &str) -> MatchmakeSession {
        let mut w = self.world.lock().unwrap();
        let gid = match w.sessions.find(criteria, &template, me.pid, count) {
            Some(gid) if self.join_existing(&mut w, me, gid, count, msg).is_ok() => gid,
            _ => {
                let gid = w.sessions.create(template, me.pid, me.cid, count);
                self.announce_join(&mut w, gid, me.pid, msg);
                gid
            }
        };
        if self.verbose {
            let s = &w.sessions.all[&gid];
            println!("napple-online: pid {} matched into gathering {gid} (mode {}, {} of {} players)", me.pid, s.mm.game_mode, s.players(), s.mm.g.max);
        }
        w.sessions.all[&gid].mm.clone()
    }
    fn session_of(&self, gid: u32) -> Option<MatchmakeSession> {
        self.world.lock().unwrap().sessions.all.get(&gid).map(|s| s.mm.clone())
    }
    fn with_session(&self, gid: u32, f: impl FnOnce(&mut MatchmakeSession)) -> Reply {
        match self.world.lock().unwrap().sessions.all.get_mut(&gid) {
            Some(s) => {
                f(&mut s.mm);
                ok(|_| {})
            }
            None => Reply::Err(err::RV_SESSION_VOID),
        }
    }

    fn matchmake_ext(&self, me: Caller, req: &Request) -> Reply {
        let mut r = Reader::new(&req.body);
        let bad = Reply::Err(err::CORE_INVALID_ARGUMENT);
        let any_session = |r: &mut Reader| -> Option<MatchmakeSession> {
            let (_, obj) = r.any().ok()?;
            MatchmakeSession::decode(&mut Reader::new(obj)).ok()
        };
        match req.method {
            1 | 2 => {
                let Ok(gid) = r.u32() else { return bad };
                let open = req.method == 2;
                self.with_session(gid, |s| s.open = open)
            }
            3 | 15 => {
                let criteria = if req.method == 15 { r.list(|r| Criteria::decode(r)).unwrap_or_default() } else { vec![] };
                let Some(t) = any_session(&mut r) else { return bad };
                let msg = r.string().unwrap_or_default();
                let s = self.auto_matchmake(me, t, &criteria, 1, &msg);
                Reply::Ok(s.to_any())
            }
            40 => {
                let parsed = (|| -> crate::nex::Result<(MatchmakeSession, String, u16, Vec<Criteria>)> {
                    let (_v, mut p) = r.structure()?;
                    let s = MatchmakeSession::decode(&mut p)?;
                    p.list(|r| r.pid())?;
                    p.u32()?;
                    p.u32()?;
                    let msg = p.string()?;
                    let count = p.u16()?;
                    let crit = p.list(|r| Criteria::decode(r))?;
                    Ok((s, msg, count, crit))
                })();
                let Ok((t, msg, count, crit)) = parsed else { return bad };
                let s = self.auto_matchmake(me, t, &crit, count, &msg);
                Reply::Ok(s.to_bytes())
            }
            6 | 38 => {
                let parsed = (|| -> crate::nex::Result<(MatchmakeSession, u16)> {
                    if req.method == 6 {
                        let (_, obj) = r.any()?;
                        let s = MatchmakeSession::decode(&mut Reader::new(obj))?;
                        r.string()?;
                        Ok((s, r.u16().unwrap_or(1)))
                    } else {
                        let (_v, mut p) = r.structure()?;
                        let s = MatchmakeSession::decode(&mut p)?;
                        p.list(|r| r.pid())?;
                        p.u32()?;
                        p.u32()?;
                        p.string()?;
                        Ok((s, p.u16().unwrap_or(1)))
                    }
                })();
                let Ok((t, count)) = parsed else { return bad };
                let mut w = self.world.lock().unwrap();
                let gid = w.sessions.create(t, me.pid, me.cid, count);
                self.announce_join(&mut w, gid, me.pid, "");
                let s = w.sessions.all[&gid].mm.clone();
                if req.method == 6 {
                    ok(|w| {
                        w.u32(gid).buffer(&s.session_key);
                    })
                } else {
                    Reply::Ok(s.to_bytes())
                }
            }
            7 | 30 | 39 => {
                let parsed = (|| -> crate::nex::Result<(u32, String, u16)> {
                    match req.method {
                        7 => Ok((r.u32()?, r.string()?, 1)),
                        30 => {
                            let gid = r.u32()?;
                            let msg = r.string()?;
                            r.bool()?;
                            Ok((gid, msg, r.u16()?))
                        }
                        _ => {
                            let (_v, mut p) = r.structure()?;
                            let gid = p.u32()?;
                            p.list(|r| r.pid())?;
                            p.u32()?;
                            p.u32()?;
                            p.u8()?;
                            p.string()?;
                            p.string()?;
                            let msg = p.string()?;
                            Ok((gid, msg, p.u16()?))
                        }
                    }
                })();
                let Ok((gid, msg, count)) = parsed else { return bad };
                let mut w = self.world.lock().unwrap();
                if let Err(e) = self.join_existing(&mut w, me, gid, count, &msg) {
                    return Reply::Err(e);
                }
                let s = w.sessions.all[&gid].mm.clone();
                if req.method == 39 {
                    Reply::Ok(s.to_bytes())
                } else {
                    ok(|w| {
                        w.buffer(&s.session_key);
                    })
                }
            }
            8 => {
                let (Ok(gid), Ok(i), Ok(v)) = (r.u32(), r.u32(), r.u32()) else { return bad };
                self.with_session(gid, |s| {
                    if let Some(a) = s.attribs.get_mut(i as usize) {
                        *a = v;
                    }
                })
            }
            12 => {
                let (Ok(gid), Ok(attribs)) = (r.u32(), r.list(|r| r.u32())) else { return bad };
                self.with_session(gid, |s| s.attribs = attribs)
            }
            14 => {
                let Some(n) = any_session(&mut r) else { return bad };
                self.with_session(n.g.id, |s| {
                    let keep = (s.g.id, s.g.owner, s.g.host, s.participation_count, s.session_key.clone(), s.started);
                    *s = n;
                    (s.g.id, s.g.owner, s.g.host, s.participation_count, s.session_key, s.started) = keep;
                })
            }
            34 => {
                let (Ok(gid), Ok(score)) = (r.u32(), r.u8()) else { return bad };
                self.with_session(gid, |s| s.progress_score = score)
            }
            41 => match r.u32().ok().and_then(|g| self.session_of(g)) {
                Some(s) => Reply::Ok(s.to_bytes()),
                None => Reply::Err(err::RV_SESSION_VOID),
            },
            48 => {
                let Ok(gids) = r.list(|r| r.u32()) else { return bad };
                let found: Vec<MatchmakeSession> = gids.iter().filter_map(|g| self.session_of(*g)).collect();
                ok(|w| {
                    w.list(&found, |w, s| s.encode(w));
                })
            }
            4 | 42 => {
                let Ok(c) = Criteria::decode(&mut r) else { return bad };
                let w = self.world.lock().unwrap();
                let found: Vec<MatchmakeSession> =
                    w.sessions.all.values().filter(|s| c.matches(&s.mm, s.free(), 1)).take(100).map(|s| s.mm.clone()).collect();
                let method = req.method;
                ok(|w| {
                    w.list(&found, |w, s| {
                        if method == 4 {
                            w.any("MatchmakeSession", &s.to_bytes());
                        } else {
                            s.encode(w);
                        }
                    });
                })
            }
            31 | 60 => {
                let Ok(pids) = r.list(|r| r.pid()) else { return bad };
                let w = self.world.lock().unwrap();
                let mut found = vec![];
                for p in pids {
                    for gid in w.sessions.of(p) {
                        let s = &w.sessions.all[&gid].mm;
                        found.push((p, gid, s.game_mode, s.attribs.first().copied().unwrap_or(0)));
                    }
                }
                ok(|w| {
                    w.list(&found, |w, (p, g, m, a)| {
                        w.structure(0, |w| {
                            w.pid(*p).u32(*g).u32(*m).u32(*a);
                        });
                    });
                })
            }
            10 | 13 | 16 | 24 => ok(|w| {
                w.u32(0);
            }),
            45 => ok(|w| {
                w.u64(crate::crypto::random_u32() as u64);
            }),
            9 | 11 | 25 | 26 | 27 | 28 | 44 | 46 | 47 => ok(|_| {}),
            _ => Reply::Err(err::CORE_NOT_IMPLEMENTED),
        }
    }

    fn match_making(&self, me: Caller, req: &Request) -> Reply {
        let mut r = Reader::new(&req.body);
        let Ok(gid) = r.u32() else { return Reply::Err(err::CORE_INVALID_ARGUMENT) };
        let mut w = self.world.lock().unwrap();
        let host_urls = |w: &World, gid: u32| -> Vec<String> {
            let Some(s) = w.sessions.all.get(&gid) else { return vec![] };
            w.clients.values().filter(|c| c.pid == s.mm.g.host).flat_map(|c| c.urls.iter().map(|u| u.to_string())).collect()
        };
        match req.method {
            2 | 33 => {
                let mine = w.sessions.all.get(&gid).is_some_and(|s| s.mm.g.owner == me.pid);
                if mine {
                    let s = w.sessions.all.remove(&gid).unwrap();
                    for p in s.participants.iter().filter(|p| p.pid != me.pid) {
                        self.notify(&mut w, p.pid, me.pid, notif::GATHERING_UNREGISTERED, gid as u64, 0, "", 0);
                    }
                }
                ok(|w| {
                    w.bool(mine);
                })
            }
            12 => {
                let msg = r.string().unwrap_or_default();
                let done = self.leave(&mut w, gid, me.pid, notif::PARTICIPATION_ENDED, &msg);
                ok(|w| {
                    w.bool(done);
                })
            }
            13 => {
                let pids: Vec<u64> = w.sessions.all.get(&gid).map(|s| s.participants.iter().map(|p| p.pid).collect()).unwrap_or_default();
                ok(|w| {
                    w.list(&pids, |w, p| {
                        w.pid(*p);
                    });
                })
            }
            16 => {
                let pids: Vec<u64> = w.sessions.all.get(&gid).map(|s| s.participants.iter().map(|p| p.pid).collect()).unwrap_or_default();
                let urls: Vec<String> =
                    w.clients.values().filter(|c| pids.contains(&c.pid)).filter_map(|c| c.urls.first().map(|u| u.to_string())).collect();
                ok(|w| {
                    w.list(&urls, |w, u| {
                        w.string(u);
                    });
                })
            }
            28 => {
                let urls = host_urls(&w, gid);
                ok(|w| {
                    w.bool(!urls.is_empty()).string(urls.first().map(|s| s.as_str()).unwrap_or(""));
                })
            }
            41 => {
                let urls = host_urls(&w, gid);
                ok(|w| {
                    w.list(&urls, |w, u| {
                        w.string(u);
                    });
                })
            }
            40 | 42 => {
                let migrate = req.method == 42 && r.bool().unwrap_or(false);
                match w.sessions.all.get_mut(&gid) {
                    Some(s) if s.participants.iter().any(|p| p.pid == me.pid) => {
                        s.mm.g.host = me.pid;
                        if migrate {
                            s.mm.g.owner = me.pid;
                        }
                        ok(|_| {})
                    }
                    _ => Reply::Err(err::RV_SESSION_VOID),
                }
            }
            43 | 44 => {
                let changed = match w.sessions.all.get_mut(&gid) {
                    Some(s) if s.participants.iter().any(|p| p.pid == me.pid) && s.mm.g.owner != me.pid => {
                        s.mm.g.owner = me.pid;
                        true
                    }
                    _ => false,
                };
                if req.method == 43 {
                    ok(|w| {
                        w.bool(changed);
                    })
                } else {
                    ok(|_| {})
                }
            }
            26 | 27 => ok(|w| {
                w.bool(true);
            }),
            _ => Reply::Err(err::CORE_NOT_IMPLEMENTED),
        }
    }

    fn match_making_ext(&self, me: Caller, req: &Request) -> Reply {
        let mut r = Reader::new(&req.body);
        let mut w = self.world.lock().unwrap();
        match req.method {
            1 => {
                let Ok(gid) = r.u32() else { return Reply::Err(err::CORE_INVALID_ARGUMENT) };
                let msg = r.string().unwrap_or_default();
                let done = self.leave(&mut w, gid, me.pid, notif::PARTICIPATION_ENDED, &msg);
                ok(|w| {
                    w.bool(done);
                })
            }
            2 | 3 => {
                let Ok(gid) = r.u32() else { return Reply::Err(err::CORE_INVALID_ARGUMENT) };
                let ps: Vec<Participant> = w.sessions.all.get(&gid).map(|s| s.participants.clone()).unwrap_or_default();
                let method = req.method;
                ok(|w| {
                    w.list(&ps, |w, p| {
                        if method == 2 {
                            w.pid(p.pid);
                        } else {
                            w.structure(0, |w| {
                                w.pid(p.pid).string(&p.pid.to_string()).string("").u16(p.count);
                            });
                        }
                    });
                })
            }
            4 => {
                let Ok(gids) = r.list(|r| r.u32()) else { return Reply::Err(err::CORE_INVALID_ARGUMENT) };
                let out: Vec<(u32, Vec<String>)> = gids
                    .iter()
                    .map(|g| {
                        let pids: Vec<u64> = w.sessions.all.get(g).map(|s| s.participants.iter().map(|p| p.pid).collect()).unwrap_or_default();
                        let urls = w.clients.values().filter(|c| pids.contains(&c.pid)).filter_map(|c| c.urls.first().map(|u| u.to_string())).collect();
                        (*g, urls)
                    })
                    .collect();
                ok(|w| {
                    w.list(&out, |w, (g, urls)| {
                        w.structure(0, |w| {
                            w.u32(*g).list(urls, |w, u| {
                                w.string(u);
                            });
                        });
                    });
                })
            }
            _ => Reply::Err(err::CORE_NOT_IMPLEMENTED),
        }
    }

    /// Ranking: common data is kept (others read it back); scores are accepted and every ranking is empty (no leaderboards on this server yet).
    fn ranking(&self, me: Caller, req: &Request) -> Reply {
        let mut r = Reader::new(&req.body);
        let empty_result = |w: &mut Writer| {
            w.structure(0, |w| {
                w.u32(0).u32(0).datetime(DateTime::now());
            });
        };
        match req.method {
            4 => {
                let Ok(data) = r.buffer() else { return Reply::Err(err::CORE_INVALID_ARGUMENT) };
                self.world.lock().unwrap().common_data.insert(me.pid, data);
                ok(|_| {})
            }
            5 => {
                self.world.lock().unwrap().common_data.remove(&me.pid);
                ok(|_| {})
            }
            6 => {
                let d = self.world.lock().unwrap().common_data.get(&me.pid).cloned().unwrap_or_default();
                ok(|w| {
                    w.buffer(&d);
                })
            }
            1 | 2 | 3 | 7 | 8 => ok(|_| {}),
            9 | 12 | 13 | 14 => ok(empty_result),
            10 => ok(|w| {
                w.u32(0);
            }),
            11 => ok(|w| {
                w.structure(0, |w| {
                    w.u32(0);
                });
            }),
            15 => ok(|w| {
                w.u32(0);
            }),
            _ => Reply::Err(err::CORE_NOT_IMPLEMENTED),
        }
    }

    /// Utility: unique ids and the server's settings tables (this server has none: empty maps).
    fn utility(&self, me: Caller, req: &Request) -> Reply {
        match req.method {
            1 | 2 => ok(|w| {
                w.u64(crate::crypto::random_u32() as u64 | (me.pid & 0xffff_ffff) << 32);
                if req.method == 2 {
                    w.u64(crate::crypto::random_u32() as u64);
                }
            }),
            3 | 4 => ok(|_| {}),
            5 => ok(|w| {
                w.u64(0).u64(0);
            }),
            6 => ok(|w| {
                w.u32(0);
            }),
            // GetIntegerSettings(u32 index) -> Map<u16, i32>; GetStringSettings(u32 index) -> Map<u16, String>
            7 | 8 => ok(|w| {
                w.u32(0);
            }),
            _ => Reply::Err(err::CORE_NOT_IMPLEMENTED),
        }
    }

    /// NAT traversal: a client asks the server to have its targets probe it; the server forwards InitiateProbe (method 2) to each target.
    fn nat_traversal(&self, me: Caller, req: &Request) -> Reply {
        let mut r = Reader::new(&req.body);
        match req.method {
            1 | 3 => {
                let Ok(targets) = r.list(|r| r.string()) else { return Reply::Err(err::CORE_INVALID_ARGUMENT) };
                let mut w = self.world.lock().unwrap();
                let probe = if req.method == 3 {
                    r.string().unwrap_or_default()
                } else {
                    w.clients.get(&me.cid).and_then(|c| c.urls.first()).map(|u| u.to_string()).unwrap_or_default()
                };
                let mut body = Writer::new();
                body.string(&probe);
                for t in targets {
                    let u = StationUrl::parse(&t);
                    let cid = u.get("RVCID").and_then(|v| v.parse::<u32>().ok());
                    let pid = u.get("PID").and_then(|v| v.parse::<u64>().ok());
                    let to: Vec<u32> =
                        w.clients.values().filter(|c| Some(c.cid) == cid || (cid.is_none() && Some(c.pid) == pid)).map(|c| c.cid).collect();
                    for c in to {
                        self.send_request(&mut w, c, PROTO_NAT_TRAVERSAL, 2, &body.buf);
                    }
                }
                ok(|_| {})
            }
            6 => ok(|w| {
                w.u32(0).datetime(DateTime::now()).string("").u16(0).u32(0).u32(0);
            }),
            4 | 5 | 7 => ok(|_| {}),
            _ => Reply::Err(err::CORE_NOT_IMPLEMENTED),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::matchmaking::tests::sample;
    use std::sync::mpsc::{channel, Receiver};

    fn auth_info(token: &str) -> Vec<u8> {
        let mut o = Writer::new();
        o.structure(0, |_| {});
        o.structure(0, |w| {
            w.string(token).u32(4).u8(2).u32(0);
        });
        o.buf
    }

    fn req(protocol: u16, method: u32, body: Vec<u8>) -> Request {
        Request { protocol, call_id: 1, method, body }
    }

    fn login(srv: &Server, method: u32) -> (u64, Vec<u8>, Vec<u8>) {
        let (a, _) = srv.accounts.create("4e6170706c6555736572000000000001").unwrap();
        let token = srv.accounts.issue_token(a.pid, "0100152000022000", now_secs());
        let mut b = Writer::new();
        if method == 2 {
            b.string(&a.pid.to_string()).any("AuthenticationInfo", &auth_info(&token));
        } else {
            b.structure(0, |w| {
                w.u32(3).string(&a.pid.to_string()).any("AuthenticationInfo", &auth_info(&token)).bool(false).u32(40302).u32(0);
            });
        }
        let mut st = AuthState::default();
        let Reply::Ok(resp) = srv.auth_call(&mut st, &req(PROTO_AUTH, method, b.buf)) else { panic!("login refused") };
        let mut r = Reader::new(&resp);
        let (pid, ticket, url, source_key);
        if method == 2 {
            assert_eq!(r.u32().unwrap(), RESULT_SUCCESS);
            pid = r.pid().unwrap();
            ticket = r.buffer().unwrap();
            let (v, mut cd) = r.structure().unwrap();
            assert_eq!(v, 1);
            url = cd.string().unwrap();
            assert_eq!(cd.u32().unwrap(), 0);
            assert_eq!(cd.string().unwrap(), "prudp:/");
            assert!(cd.datetime().unwrap().year() >= 2024);
            assert!(r.string().unwrap().starts_with("napple-online"));
            source_key = r.string().unwrap();
        } else {
            let (_, mut s) = r.structure().unwrap();
            pid = s.pid().unwrap();
            ticket = s.buffer().unwrap();
            url = s.string().unwrap();
            s.datetime().unwrap();
            s.string().unwrap();
            source_key = s.string().unwrap();
        }
        assert_eq!(pid, a.pid);
        assert_eq!(url, secure_station_url());
        let key = crate::crypto::unhex(&source_key).unwrap();
        let (session, target, internal) = kerberos::open_client_ticket(&key, &ticket).unwrap();
        assert_eq!(target, SECURE_PID);
        // RequestTicket on the same connection gives a fresh ticket under the same source key
        let mut b = Writer::new();
        b.pid(pid).pid(SECURE_PID);
        let Reply::Ok(rt) = srv.auth_call(&mut st, &req(PROTO_AUTH, 3, b.buf)) else { panic!() };
        let mut r = Reader::new(&rt);
        assert_eq!(r.u32().unwrap(), RESULT_SUCCESS);
        assert!(kerberos::open_client_ticket(&key, &r.buffer().unwrap()).is_ok());
        (pid, session, internal)
    }

    #[test]
    fn authentication_login_and_ticket() {
        let srv = Server::new(Accounts::open(None).unwrap(), false);
        for m in [2, 6] {
            let (pid, session, internal) = login(&srv, m);
            // the secure server accepts the internal ticket with the session key
            let c = kerberos::open_secure_connect(&srv.secure_key, &kerberos::client_secure_connect(&internal, &session, pid, 5, 9), now_secs()).unwrap();
            assert_eq!(c.pid, pid);
        }
        // a bad token is refused
        let mut b = Writer::new();
        b.string("1").any("AuthenticationInfo", &auth_info("eyJhbGciOiJIUzI1NiJ9.e30.AAAA"));
        assert!(matches!(srv.auth_call(&mut AuthState::default(), &req(PROTO_AUTH, 2, b.buf)), Reply::Err(err::RV_ACCOUNT_EXPIRED)));
        assert!(matches!(srv.auth_call(&mut AuthState::default(), &req(PROTO_AUTH, 1, vec![])), Reply::Err(_)));
        // RequestTicket before login is refused
        let mut b = Writer::new();
        b.pid(5).pid(SECURE_PID);
        assert!(matches!(srv.auth_call(&mut AuthState::default(), &req(PROTO_AUTH, 3, b.buf)), Reply::Err(err::CORE_ACCESS_DENIED)));
    }

    fn attach(srv: &Server, pid: u64) -> (Caller, Receiver<Vec<u8>>) {
        let (tx, rx) = channel();
        let cid = srv.attach(pid, "192.168.1.20".parse().unwrap(), tx);
        (Caller { pid, cid }, rx)
    }

    #[test]
    fn secure_register() {
        let srv = Server::new(Accounts::open(None).unwrap(), false);
        let (me, _rx) = attach(&srv, 77);
        let mut b = Writer::new();
        b.list(&["prudp:/address=10.0.0.2;port=49152;natf=0;natm=0;pmp=0;sid=15;type=2;upnp=0"], |w, s| {
            w.string(s);
        });
        b.any("NintendoLoginData", &auth_info("t"));
        let Reply::Ok(resp) = srv.secure_call(me, &req(PROTO_SECURE, 4, b.buf)) else { panic!() };
        let mut r = Reader::new(&resp);
        assert_eq!(r.u32().unwrap(), RESULT_SUCCESS);
        assert_eq!(r.u32().unwrap(), me.cid);
        let public = StationUrl::parse(&r.string().unwrap());
        assert_eq!(public.get("address"), Some("192.168.1.20"));
        assert_ne!(crate::natcheck::observed_ip("127.0.0.1".parse().unwrap()).to_string(), "", "a loopback peer is reported at the LAN address when there is one");
        assert_eq!(public.get("port"), Some("49152"));
        assert_eq!(public.get("RVCID"), Some(me.cid.to_string().as_str()));
        // another client finds the URLs by pid
        let (other, _rx2) = attach(&srv, 78);
        let mut b = Writer::new();
        b.u32(0).pid(77);
        let Reply::Ok(resp) = srv.secure_call(other, &req(PROTO_SECURE, 3, b.buf)) else { panic!() };
        let mut r = Reader::new(&resp);
        assert!(r.bool().unwrap());
        assert_eq!(r.list(|r| r.string()).unwrap().len(), 1);
    }

    fn notification(rx: &Receiver<Vec<u8>>) -> (u64, u32, u64, u64) {
        let m = rx.try_recv().expect("a notification");
        let (rmc::Message::Request(q), _) = rmc::decode(&m).unwrap() else { panic!() };
        assert_eq!((q.protocol, q.method), (PROTO_NOTIFICATION, 1));
        let mut r = Reader::new(&q.body);
        let (_, mut e) = r.structure().unwrap();
        (e.pid().unwrap(), e.u32().unwrap(), e.u64().unwrap(), e.u64().unwrap())
    }

    #[test]
    fn two_players_matched_into_one_session() {
        let srv = Server::new(Accounts::open(None).unwrap(), true);
        let (a, rx_a) = attach(&srv, 100);
        let (b, _rx_b) = attach(&srv, 200);
        let call = |me: Caller| -> MatchmakeSession {
            let mut body = Writer::new();
            body.list(&[0], |w, _| {
                w.structure(0, |w| {
                    w.list(&[""], |w, s: &&str| {
                        w.string(s);
                    });
                    w.string("3").string("").string("").string("").bool(true).bool(false).bool(false).u32(0);
                });
            });
            body.any("MatchmakeSession", &sample(3, 12).to_bytes()).string("hi");
            let Reply::Ok(resp) = srv.secure_call(me, &req(PROTO_MATCHMAKE_EXT, 15, body.buf)) else { panic!() };
            let mut r = Reader::new(&resp);
            let (name, obj) = r.any().unwrap();
            assert_eq!(name, "MatchmakeSession");
            MatchmakeSession::decode(&mut Reader::new(obj)).unwrap()
        };
        let s1 = call(a);
        assert_eq!((s1.g.owner, s1.g.host, s1.participation_count), (100, 100, 1));
        let s2 = call(b);
        assert_eq!(s2.g.id, s1.g.id, "the second player joins the first one's session");
        assert_eq!(s2.participation_count, 2);
        assert_eq!(s2.session_key, s1.session_key);
        // the owner hears about the new participant
        assert_eq!(notification(&rx_a), (200, notif::PARTICIPANT_JOINED, s1.g.id as u64, 200));
        // GetSessionURLs gives the host's registered URLs
        let mut reg = Writer::new();
        reg.list(&["prudp:/address=10.0.0.2;port=49152;sid=15;type=2"], |w, s| {
            w.string(s);
        });
        srv.secure_call(a, &req(PROTO_SECURE, 1, reg.buf));
        let mut q = Writer::new();
        q.u32(s1.g.id);
        let Reply::Ok(resp) = srv.secure_call(b, &req(PROTO_MATCH_MAKING, 41, q.buf.clone())) else { panic!() };
        let urls = Reader::new(&resp).list(|r| r.string()).unwrap();
        assert_eq!(urls.len(), 1);
        assert!(urls[0].contains("address=10.0.0.2"));
        // NAT traversal: b asks a to probe it
        let mut nt = Writer::new();
        nt.list(&[urls[0].clone()], |w, s| {
            w.string(s);
        });
        nt.string("prudp:/address=10.0.0.3;port=49153");
        srv.secure_call(b, &req(PROTO_NAT_TRAVERSAL, 3, nt.buf));
        let m = rx_a.try_recv().unwrap();
        let (rmc::Message::Request(probe), _) = rmc::decode(&m).unwrap() else { panic!() };
        assert_eq!((probe.protocol, probe.method), (PROTO_NAT_TRAVERSAL, 2));
        // b leaves: a (the owner) is told
        let mut end = Writer::new();
        end.u32(s1.g.id).string("bye");
        srv.secure_call(b, &req(PROTO_MATCH_MAKING_EXT, 1, end.buf));
        assert_eq!(notification(&rx_a).1, notif::PARTICIPATION_ENDED);
        // a disconnects: the session is gone
        srv.detach(a.cid);
        assert!(srv.world.lock().unwrap().sessions.all.is_empty());
        // unknown protocols answer NotImplemented; Utility settings are empty maps
        assert!(matches!(srv.secure_call(b, &req(115, 1, vec![])), Reply::Err(err::CORE_NOT_IMPLEMENTED)));
        let Reply::Ok(m) = srv.secure_call(b, &req(PROTO_UTILITY, 7, vec![0, 0, 0, 0])) else { panic!() };
        assert_eq!(m, vec![0, 0, 0, 0]);
    }

    #[test]
    fn verbose_gatherings_announce_every_join_to_everyone() {
        let srv = Server::new(Accounts::open(None).unwrap(), false);
        let (a, rx_a) = attach(&srv, 100);
        let (b, rx_b) = attach(&srv, 200);
        let auto = |me: Caller| -> MatchmakeSession {
            let mut t = sample(1, 12);
            t.g.flags = 0xa10; // MK8D's worldwide gathering
            let mut body = Writer::new();
            body.structure(0, |w| {
                t.encode(w);
                w.u32(0).u32(0).u32(0).string("").u16(1).u32(0).u32(0).structure(0, |w| {
                    w.u32(0);
                });
            });
            let Reply::Ok(resp) = srv.secure_call(me, &req(PROTO_MATCHMAKE_EXT, 40, body.buf)) else { panic!() };
            MatchmakeSession::decode(&mut Reader::new(&resp)).unwrap()
        };
        let s1 = auto(a);
        assert_eq!(s1.g.owner, 100);
        // the creator hears about its own join
        assert_eq!(notification(&rx_a), (100, notif::PARTICIPANT_JOINED, s1.g.id as u64, 100));
        assert!(rx_a.try_recv().is_err());
        let s2 = auto(b);
        assert_eq!(s2.g.id, s1.g.id);
        // the second join reaches both
        assert_eq!(notification(&rx_a), (200, notif::PARTICIPANT_JOINED, s1.g.id as u64, 200));
        assert_eq!(notification(&rx_b), (200, notif::PARTICIPANT_JOINED, s1.g.id as u64, 200));
    }

    #[test]
    fn token_extraction() {
        assert_eq!(token_from_extra(&auth_info("eyJx.eyJy.zz")).as_deref(), Some("eyJx.eyJy.zz"));
        assert_eq!(token_from_extra(b"\x00junk eyJa.b-c_d.e\x00").as_deref(), Some("eyJa.b-c_d.e"));
        assert_eq!(token_from_extra(b"nothing"), None);
    }
}
