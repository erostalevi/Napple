//! One client WebSocket: the PRUDP connections inside it (authentication port and secure port), RMC dispatch to the server, and notifications
//! back to the client. I/O-free (bytes in, bytes out, explicit time) so the whole flow is testable; `serve` drives it over a TcpStream.

use crate::accounts::now_secs;
use crate::crypto::random_bytes;
use crate::http::{self, WsReader, WS_BINARY, WS_CLOSE, WS_PING, WS_PONG};
use crate::kerberos;
use crate::prudp::{self, Connection, Packet, State};
use crate::rmc::{self, Message};
use crate::server::{AuthState, Caller, Reply, Server, PORT_AUTH, PORT_SECURE};
use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::{IpAddr, TcpStream};
use std::sync::mpsc::{channel, Receiver, Sender};
use std::sync::Arc;
use std::time::{Duration, Instant};

enum Role {
    Auth(AuthState),
    Secure(Option<Caller>),
}

struct Slot {
    conn: Connection,
    role: Role,
}

pub struct NexSession {
    server: Arc<Server>,
    peer: IpAddr,
    slots: HashMap<(u8, u8), Slot>,
    tx: Sender<Vec<u8>>,
    rx: Receiver<Vec<u8>>,
    pub closed: bool,
}

fn proto_name(p: u16) -> &'static str {
    match p {
        3 => "NATTraversal",
        10 => "Authentication",
        11 => "SecureConnection",
        14 => "Notification",
        18 => "Health",
        21 => "MatchMaking",
        50 => "MatchMakingExt",
        109 => "MatchmakeExtension",
        110 => "Utility",
        112 => "Ranking",
        115 => "DataStore",
        _ => "?",
    }
}

impl NexSession {
    pub fn new(server: Arc<Server>, peer: IpAddr) -> NexSession {
        let (tx, rx) = channel();
        NexSession { server, peer, slots: HashMap::new(), tx, rx, closed: false }
    }

    fn signature(&self) -> [u8; 16] {
        let mut s = [0u8; 16];
        s.copy_from_slice(&random_bytes(16));
        s
    }

    /// One WebSocket binary message from the client. Returns the messages to send back (each one PRUDP packet).
    pub fn on_message(&mut self, data: &[u8], now_ms: u64) -> Vec<Vec<u8>> {
        let mut out = Vec::new();
        if self.server.verbose {
            eprintln!("napple-online: {} <- {}", self.peer, crate::crypto::hex(&data[..data.len().min(96)]));
        }
        let packets = prudp::decode_all(data);
        if packets.is_empty() {
            eprintln!("napple-online: {}: undecodable PRUDP message ({} bytes, first byte 0x{:02x})", self.peer, data.len(), data.first().copied().unwrap_or(0));
        }
        for p in packets {
            self.on_packet(&p, now_ms, &mut out);
        }
        out.extend(self.tick(now_ms));
        if self.server.verbose {
            for m in &out {
                eprintln!("napple-online: {} -> {}", self.peer, crate::crypto::hex(&m[..m.len().min(96)]));
            }
        }
        out
    }

    fn on_packet(&mut self, p: &Packet, now_ms: u64, out: &mut Vec<Vec<u8>>) {
        let key = (p.dst_port, p.src_port);
        if p.ptype == prudp::TYPE_SYN && !p.has(prudp::FLAG_ACK) {
            // a new SYN starts the connection over
            if let Some(Slot { role: Role::Secure(Some(c)), .. }) = self.slots.remove(&key) {
                self.server.detach(c.cid);
            }
            if p.dst_port != PORT_AUTH && p.dst_port != PORT_SECURE {
                eprintln!("napple-online: {}: SYN for unknown port {}", self.peer, p.dst_port);
                return;
            }
            let role = if p.dst_port == PORT_AUTH { Role::Auth(AuthState::default()) } else { Role::Secure(None) };
            let sig = self.signature();
            self.slots.insert(key, Slot { conn: Connection::new(p.dst_port, p.src_port, sig), role });
        }
        let Some(slot) = self.slots.get_mut(&key) else { return };
        let server = self.server.clone();
        let peer = self.peer;
        let tx = self.tx.clone();
        let mut attached: Option<Caller> = None;
        let is_secure = matches!(slot.role, Role::Secure(_));
        let ev = slot.conn.receive(p, now_ms, &mut |payload| {
            if !is_secure {
                return Some(vec![]);
            }
            match kerberos::open_secure_connect(&server.secure_key, payload, now_secs()) {
                Ok(c) => {
                    let cid = server.attach(c.pid, peer, tx.clone());
                    attached = Some(Caller { pid: c.pid, cid });
                    eprintln!("napple-online: {peer}: account {} connected to the secure server (connection {cid})", c.pid);
                    Some(kerberos::secure_connect_response(c.check))
                }
                Err(e) => {
                    eprintln!("napple-online: {peer}: secure CONNECT refused: {e}");
                    None
                }
            }
        });
        if let Some(c) = attached {
            slot.role = Role::Secure(Some(c));
        }
        out.extend(ev.out.iter().map(|q| q.encode()));
        for m in ev.messages {
            self.dispatch(key, &m, now_ms, out);
        }
        if ev.closed {
            self.drop_slot(key);
        }
    }

    fn dispatch(&mut self, key: (u8, u8), payload: &[u8], now_ms: u64, out: &mut Vec<Vec<u8>>) {
        let mut at = 0;
        while at < payload.len() {
            let (msg, used) = match rmc::decode(&payload[at..]) {
                Ok(v) => v,
                Err(e) => {
                    eprintln!("napple-online: {}: undecodable RMC message: {e}", self.peer);
                    return;
                }
            };
            at += used;
            let Message::Request(req) = msg else { continue };
            let Some(slot) = self.slots.get_mut(&key) else { return };
            let reply = match &mut slot.role {
                Role::Auth(st) => self.server.auth_call(st, &req),
                Role::Secure(Some(c)) => self.server.secure_call(*c, &req),
                Role::Secure(None) => Reply::Err(rmc::err::CORE_ACCESS_DENIED),
            };
            let (bytes, status) = match reply {
                Reply::Ok(data) => (rmc::encode_success(req.protocol, req.call_id, req.method, &data), "ok".to_string()),
                Reply::Err(code) => (rmc::encode_error(req.protocol, req.call_id, code), format!("error 0x{:08x}", code | 0x8000_0000)),
            };
            if self.server.verbose || status != "ok" {
                eprintln!("napple-online: {}: {} ({}) method {} -> {status}", self.peer, proto_name(req.protocol), req.protocol, req.method);
            }
            for p in slot.conn.send(&bytes, now_ms) {
                out.push(p.encode());
            }
        }
    }

    fn drop_slot(&mut self, key: (u8, u8)) {
        if let Some(Slot { role: Role::Secure(Some(c)), .. }) = self.slots.remove(&key) {
            self.server.detach(c.cid);
            eprintln!("napple-online: {}: account {} left (connection {})", self.peer, c.pid, c.cid);
        }
    }

    /// Timers and notifications: resends overdue packets, delivers queued server requests. Drops connections whose client stopped answering.
    pub fn tick(&mut self, now_ms: u64) -> Vec<Vec<u8>> {
        let mut out = Vec::new();
        while let Ok(m) = self.rx.try_recv() {
            if let Some(slot) = self.slots.values_mut().find(|s| matches!(s.role, Role::Secure(Some(_))) && s.conn.state == State::Connected) {
                out.extend(slot.conn.send(&m, now_ms).iter().map(|p| p.encode()));
            }
        }
        let mut dead = vec![];
        for (k, s) in self.slots.iter_mut() {
            match s.conn.resend_due(now_ms) {
                Ok(v) => out.extend(v.iter().map(|p| p.encode())),
                Err(()) => dead.push(*k),
            }
        }
        for k in dead {
            self.drop_slot(k);
        }
        out
    }

    pub fn close(&mut self) {
        let keys: Vec<_> = self.slots.keys().copied().collect();
        for k in keys {
            self.drop_slot(k);
        }
        self.closed = true;
    }
}

/// Serves one TCP connection: the account API, or a WebSocket upgrade into the game server.
pub fn serve(server: Arc<Server>, mut stream: TcpStream) {
    let peer = stream.peer_addr().map(|a| a.ip()).unwrap_or(IpAddr::from([0, 0, 0, 0]));
    let _ = stream.set_read_timeout(Some(Duration::from_secs(30)));
    let (req, extra) = match http::read_request(&mut stream) {
        Ok(v) => v,
        Err(_) => return,
    };
    if !req.is_websocket() {
        let (st, reason, body) = http::handle_api(&server.accounts, &req);
        if server.verbose || st != 200 {
            eprintln!("napple-online: {peer}: {} {} -> {st}", req.method, req.path);
        }
        let _ = http::write_response(&mut stream, st, reason, &body);
        return;
    }
    eprintln!("napple-online: {peer}: game connection (WebSocket, host '{}', path '{}')", req.header("host").unwrap_or(""), req.path);
    if stream.write_all(http::websocket_accept(&req).as_bytes()).is_err() {
        return;
    }
    let _ = stream.set_nodelay(true);
    let _ = stream.set_read_timeout(Some(Duration::from_millis(100)));
    let start = Instant::now();
    let now = || start.elapsed().as_millis() as u64;
    let mut session = NexSession::new(server, peer);
    let mut ws = WsReader::default();
    ws.feed(&extra);
    let mut buf = [0u8; 16384];
    let mut last_heard = now();
    'run: loop {
        loop {
            match ws.next() {
                Ok(Some((op, payload))) => {
                    last_heard = now();
                    let replies = match op {
                        WS_BINARY | 1 => session.on_message(&payload, now()),
                        WS_PING => {
                            let _ = stream.write_all(&http::ws_frame(WS_PONG, &payload));
                            vec![]
                        }
                        WS_CLOSE => {
                            let _ = stream.write_all(&http::ws_frame(WS_CLOSE, &payload));
                            break 'run;
                        }
                        _ => vec![],
                    };
                    for r in replies {
                        if stream.write_all(&http::ws_frame(WS_BINARY, &r)).is_err() {
                            break 'run;
                        }
                    }
                }
                Ok(None) => break,
                Err(e) => {
                    eprintln!("napple-online: {peer}: WebSocket error: {e}");
                    break 'run;
                }
            }
        }
        for r in session.tick(now()) {
            if stream.write_all(&http::ws_frame(WS_BINARY, &r)).is_err() {
                break 'run;
            }
        }
        if now() - last_heard > 60_000 {
            eprintln!("napple-online: {peer}: silent for a minute, closing");
            break;
        }
        match stream.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => ws.feed(&buf[..n]),
            Err(e) if matches!(e.kind(), std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut) => {}
            Err(_) => break,
        }
    }
    session.close();
    eprintln!("napple-online: {peer}: game connection closed");
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::accounts::Accounts;
    use crate::matchmaking::{tests::sample, MatchmakeSession};
    use crate::nex::{Reader, Writer};
    use crate::prudp::*;
    use crate::server::{RESULT_SUCCESS, SECURE_PID};

    /// A minimal title-side PRUDP Lite client, enough to walk the login.
    struct TestClient {
        port: u8,
        seq: u16,
    }
    impl TestClient {
        fn pkt(&mut self, dst: u8, ptype: u8, flags: u16, payload: Vec<u8>) -> Vec<u8> {
            let p = Packet { src_type: 10, dst_type: 10, src_port: self.port, dst_port: dst, ptype, flags, seq: self.seq, payload, ..Default::default() };
            p.encode()
        }
        fn handshake(&mut self, s: &mut NexSession, dst: u8, connect_payload: Vec<u8>) -> Packet {
            self.seq = 0;
            let syn = self.pkt(dst, TYPE_SYN, FLAG_NEED_ACK, vec![]);
            let r = decode_all(&s.on_message(&syn, 0).concat());
            assert_eq!(r[0].ptype, TYPE_SYN);
            assert!(r[0].has(FLAG_ACK));
            self.seq = 1;
            let con = self.pkt(dst, TYPE_CONNECT, FLAG_RELIABLE | FLAG_NEED_ACK, connect_payload);
            let r = decode_all(&s.on_message(&con, 0).concat());
            self.seq = 2;
            r.into_iter().next().expect("CONNECT-ACK")
        }
        /// Sends an RMC request and returns the RMC response payload (acking what the server sent).
        fn call(&mut self, s: &mut NexSession, dst: u8, protocol: u16, method: u32, body: &[u8]) -> Vec<u8> {
            let msg = rmc::encode_request(protocol, self.seq as u32, method, body);
            let d = self.pkt(dst, TYPE_DATA, FLAG_RELIABLE | FLAG_NEED_ACK, msg);
            self.seq += 1;
            let r = decode_all(&s.on_message(&d, 0).concat());
            assert!(r.iter().any(|p| p.has(FLAG_ACK)), "the request is acked");
            let mut data = Vec::new();
            for p in r.iter().filter(|p| p.ptype == TYPE_DATA && p.has(FLAG_RELIABLE)) {
                data.extend_from_slice(&p.payload);
                let ack = Packet { src_type: 10, dst_type: 10, src_port: self.port, dst_port: dst, ptype: TYPE_DATA, flags: FLAG_ACK, seq: p.seq, ..Default::default() };
                s.on_message(&ack.encode(), 0);
            }
            match rmc::decode(&data).unwrap().0 {
                Message::Response { result: Ok((m, d)), .. } => {
                    assert_eq!(m, method);
                    d
                }
                other => panic!("call failed: {other:?}"),
            }
        }
    }

    fn login(srv: &Arc<Server>, uid: &str, port: u8) -> (NexSession, TestClient, u64) {
        let (a, _) = srv.accounts.create(uid).unwrap();
        let token = srv.accounts.issue_token(a.pid, "0100152000022000", now_secs());
        let mut s = NexSession::new(srv.clone(), "127.0.0.1".parse().unwrap());
        let mut c = TestClient { port, seq: 0 };
        let ack = c.handshake(&mut s, PORT_AUTH, vec![]);
        assert_eq!(ack.ptype, TYPE_CONNECT);
        let mut info = Writer::new();
        info.structure(0, |_| {});
        info.structure(0, |w| {
            w.string(&token).u32(4).u8(2).u32(0);
        });
        let mut body = Writer::new();
        body.string(&a.pid.to_string()).any("AuthenticationInfo", &info.buf);
        let resp = c.call(&mut s, PORT_AUTH, 10, 2, &body.buf);
        let mut r = Reader::new(&resp);
        assert_eq!(r.u32().unwrap(), RESULT_SUCCESS);
        assert_eq!(r.pid().unwrap(), a.pid);
        let ticket = r.buffer().unwrap();
        r.structure().unwrap();
        r.string().unwrap();
        let source_key = crate::crypto::unhex(&r.string().unwrap()).unwrap();
        let (session_key, target, internal) = kerberos::open_client_ticket(&source_key, &ticket).unwrap();
        assert_eq!(target, SECURE_PID);
        // secure server, same WebSocket, port 2
        let ack = c.handshake(&mut s, PORT_SECURE, kerberos::client_secure_connect(&internal, &session_key, a.pid, 1, 0x55));
        assert_eq!(ack.ptype, TYPE_CONNECT);
        assert_eq!(ack.payload, kerberos::secure_connect_response(0x55));
        let mut reg = Writer::new();
        reg.list(&["prudp:/address=192.168.0.9;port=50000;natf=0;natm=0;pmp=0;sid=15;type=2;upnp=0"], |w, u| {
            w.string(u);
        });
        reg.any("NintendoLoginData", &info.buf);
        let resp = c.call(&mut s, PORT_SECURE, 11, 4, &reg.buf);
        assert_eq!(Reader::new(&resp).u32().unwrap(), RESULT_SUCCESS);
        (s, c, a.pid)
    }

    #[test]
    fn full_login_and_matchmaking_over_prudp() {
        let srv = Arc::new(Server::new(Accounts::open(None).unwrap(), false));
        let (mut s1, mut c1, pid1) = login(&srv, "4e6170706c6555736572000000000001", 0x1f);
        let (mut s2, mut c2, pid2) = login(&srv, "4e6170706c6555736572000000000002", 0x1f);
        assert_ne!(pid1, pid2);
        let auto = |s: &mut NexSession, c: &mut TestClient| -> MatchmakeSession {
            let mut b = Writer::new();
            b.structure(0, |w| {
                sample(3, 12).encode(w);
                w.u32(0).u32(0).u32(0).string("").u16(1).u32(0).u32(0).structure(0, |w| {
                    w.u32(0);
                });
            });
            let resp = c.call(s, PORT_SECURE, 109, 40, &b.buf);
            MatchmakeSession::decode(&mut Reader::new(&resp)).unwrap()
        };
        let m1 = auto(&mut s1, &mut c1);
        let m2 = auto(&mut s2, &mut c2);
        assert_eq!(m1.g.id, m2.g.id, "both instances are in the same room");
        assert_eq!(m2.participation_count, 2);
        // the owner's session gets the join notification on its next tick
        let out = decode_all(&s1.tick(0).concat());
        let n = out.iter().find(|p| p.ptype == TYPE_DATA && p.has(FLAG_RELIABLE)).expect("notification packet");
        let (Message::Request(q), _) = rmc::decode(&n.payload).unwrap() else { panic!() };
        assert_eq!((q.protocol, q.method), (14, 1));
        // the second client vanishes: its connection is dropped and it leaves the room
        s2.close();
        assert_eq!(srv.world.lock().unwrap().sessions.all[&m1.g.id].participants.len(), 1);
    }

    #[test]
    fn secure_connect_with_a_forged_ticket_is_refused() {
        let srv = Arc::new(Server::new(Accounts::open(None).unwrap(), false));
        let mut s = NexSession::new(srv.clone(), "127.0.0.1".parse().unwrap());
        let mut c = TestClient { port: 0x1f, seq: 0 };
        let syn = c.pkt(PORT_SECURE, TYPE_SYN, FLAG_NEED_ACK, vec![]);
        s.on_message(&syn, 0);
        c.seq = 1;
        let forged = kerberos::client_secure_connect(&[0; 40], &[0; 32], 1, 1, 1);
        let con = c.pkt(PORT_SECURE, TYPE_CONNECT, FLAG_RELIABLE | FLAG_NEED_ACK, forged);
        assert!(s.on_message(&con, 0).is_empty());
        assert!(srv.world.lock().unwrap().clients.is_empty());
    }
}
