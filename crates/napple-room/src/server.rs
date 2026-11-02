//! The room server: rooms of Napple instances that relay virtual-LAN and LDN traffic to each other.
//!
//! One thread reads each client, one writes to it (through a bounded queue, so a slow client never holds the others up: its relayed packets are dropped
//! when its queue is full, and it is disconnected when even a control message does not fit). A room is created by its first member (who sets its
//! password) and goes away with its last. Members get a virtual IPv4 address in the configured subnet; a `Packet` must come from the sender's own
//! address and goes to one member, or to every other member when it is addressed to a broadcast address. Each member is rate limited (bytes and
//! packets per second, token buckets); a member silent for longer than the keepalive is dropped.

use crate::proto::{self, Member, Msg, ProtoError, RoomInfo, err};
use std::collections::{BTreeMap, HashMap};
use std::io::{self, Write};
use std::net::{Shutdown, SocketAddr, TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc::{Receiver, SyncSender, TrySendError, sync_channel};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

#[derive(Clone, Debug)]
pub struct Config {
    pub listen: SocketAddr,
    /// Virtual LAN subnet (network, prefix length).
    pub subnet: (u32, u8),
    pub max_members: usize,
    pub max_rooms: usize,
    pub max_clients: usize,
    /// Relayed bytes per second per member (sustained), and the burst on top.
    pub rate_bytes: u64,
    pub burst_bytes: u64,
    /// Relayed packets per second per member, and the burst.
    pub rate_packets: u64,
    pub burst_packets: u64,
    pub keepalive: Duration,
    pub hello_timeout: Duration,
    /// Frames queued for one member before its relayed packets are dropped.
    pub queue: usize,
    pub quiet: bool,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            listen: SocketAddr::from(([0, 0, 0, 0], proto::DEFAULT_PORT)),
            subnet: (0x0a0d_0000, 16),
            max_members: 16,
            max_rooms: 256,
            max_clients: 1024,
            rate_bytes: 2 * 1024 * 1024,
            burst_bytes: 512 * 1024,
            rate_packets: 4000,
            burst_packets: 1000,
            keepalive: Duration::from_secs(30),
            hello_timeout: Duration::from_secs(10),
            queue: 1024,
            quiet: false,
        }
    }
}

/// Counters, for tests and the log.
#[derive(Default, Debug)]
pub struct Stats {
    pub relayed: AtomicU64,
    pub dropped_rate: AtomicU64,
    pub dropped_queue: AtomicU64,
    pub dropped_spoof: AtomicU64,
    pub dropped_unroutable: AtomicU64,
}

type Frame = Arc<Vec<u8>>;

struct Peer {
    info: Member,
    tx: SyncSender<Frame>,
    kill: TcpStream,
    ldn: Option<(u64, Vec<u8>)>,
}

struct Room {
    password: String,
    members: BTreeMap<u32, Peer>,
}

#[derive(Default)]
struct State {
    rooms: HashMap<String, Room>,
    clients: usize,
    next_member: u32,
}

struct Shared {
    cfg: Config,
    state: Mutex<State>,
    stats: Stats,
    stop: AtomicBool,
}

pub struct Server {
    listener: TcpListener,
    shared: Arc<Shared>,
}

/// A running server (see `Server::spawn`). Dropping it does not stop the server; call `shutdown`.
pub struct Handle {
    addr: SocketAddr,
    shared: Arc<Shared>,
    thread: Option<thread::JoinHandle<()>>,
}

impl Handle {
    pub fn addr(&self) -> SocketAddr {
        self.addr
    }
    pub fn stats(&self) -> &Stats {
        &self.shared.stats
    }
    /// Number of rooms and connected members.
    pub fn census(&self) -> (usize, usize) {
        let st = self.shared.state.lock().unwrap();
        (st.rooms.len(), st.rooms.values().map(|r| r.members.len()).sum())
    }
    pub fn shutdown(mut self) {
        self.shared.stop.store(true, Ordering::SeqCst);
        let _ = TcpStream::connect(self.addr); // wakes the accept loop
        let st = self.shared.state.lock().unwrap();
        for r in st.rooms.values() {
            for p in r.members.values() {
                let _ = p.kill.shutdown(Shutdown::Both);
            }
        }
        drop(st);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

macro_rules! log {
    ($sh:expr, $($a:tt)*) => {
        if !$sh.cfg.quiet {
            eprintln!("[napple-room] {}", format!($($a)*));
        }
    };
}

impl Server {
    pub fn bind(cfg: Config) -> io::Result<Server> {
        let listener = TcpListener::bind(cfg.listen)?;
        Ok(Server { listener, shared: Arc::new(Shared { cfg, state: Mutex::new(State::default()), stats: Stats::default(), stop: AtomicBool::new(false) }) })
    }

    pub fn local_addr(&self) -> io::Result<SocketAddr> {
        self.listener.local_addr()
    }

    /// Serves on the calling thread until `stop` is set by a `Handle`.
    pub fn run(self) {
        let sh = self.shared;
        log!(sh, "listening on {} (subnet {}/{}, {} members per room)", self.listener.local_addr().map(|a| a.to_string()).unwrap_or_default(),
            proto::ip_str(sh.cfg.subnet.0), sh.cfg.subnet.1, sh.cfg.max_members);
        for conn in self.listener.incoming() {
            if sh.stop.load(Ordering::SeqCst) {
                break;
            }
            let Ok(stream) = conn else { continue };
            let sh2 = sh.clone();
            let _ = thread::Builder::new().name("napple-room-client".into()).spawn(move || client(sh2, stream));
        }
    }

    pub fn spawn(self) -> io::Result<Handle> {
        let addr = self.listener.local_addr()?;
        let shared = self.shared.clone();
        let thread = thread::Builder::new().name("napple-room-accept".into()).spawn(move || self.run())?;
        Ok(Handle { addr, shared, thread: Some(thread) })
    }
}

// ---- one client ---------------------------------------------------------------------------------------------------------------------------------------
fn send_now(stream: &mut TcpStream, m: &Msg) {
    let _ = stream.write_all(&m.encode());
}
fn refuse(stream: &mut TcpStream, code: u32, message: &str) {
    send_now(stream, &Msg::Error { code, message: message.into() });
    let _ = stream.shutdown(Shutdown::Both);
}

fn valid_room_name(s: &str) -> bool {
    !s.is_empty() && s.len() <= 64 && s.chars().all(|c| c.is_ascii_alphanumeric() || " _.-".contains(c))
}
fn clean_nick(s: &str) -> String {
    let n: String = s.chars().filter(|c| !c.is_control()).take(32).collect();
    if n.trim().is_empty() { "player".into() } else { n }
}
fn same_secret(a: &str, b: &str) -> bool {
    let (a, b) = (a.as_bytes(), b.as_bytes());
    let mut d = (a.len() ^ b.len()) as u8 | (a.len() != b.len()) as u8;
    for i in 0..a.len().max(b.len()) {
        d |= a.get(i).copied().unwrap_or(0) ^ b.get(i).copied().unwrap_or(0);
    }
    d == 0
}

/// Addresses a member may get: the subnet's hosts minus the network, `.1` (kept for a gateway) and the broadcast.
fn host_range(subnet: (u32, u8)) -> (u32, u32) {
    let mask = proto::netmask(subnet.1);
    (subnet.0 + 2, (subnet.0 | !mask) - 1)
}
fn broadcast_of(subnet: (u32, u8)) -> u32 {
    subnet.0 | !proto::netmask(subnet.1)
}

struct Bucket {
    tokens: f64,
    rate: f64,
    burst: f64,
    at: Instant,
}
impl Bucket {
    fn new(rate: u64, burst: u64) -> Self {
        Bucket { tokens: burst as f64, rate: rate as f64, burst: burst as f64, at: Instant::now() }
    }
    fn take(&mut self, n: f64) -> bool {
        let now = Instant::now();
        self.tokens = (self.tokens + now.duration_since(self.at).as_secs_f64() * self.rate).min(self.burst);
        self.at = now;
        if self.tokens >= n {
            self.tokens -= n;
            true
        } else {
            false
        }
    }
}

fn writer(mut stream: TcpStream, rx: Receiver<Frame>) {
    while let Ok(f) = rx.recv() {
        if stream.write_all(&f).is_err() {
            break;
        }
    }
    let _ = stream.shutdown(Shutdown::Both);
}

fn client(sh: Arc<Shared>, mut stream: TcpStream) {
    let peer = stream.peer_addr().map(|a| a.to_string()).unwrap_or_default();
    let _ = stream.set_nodelay(true);
    let _ = stream.set_read_timeout(Some(sh.cfg.hello_timeout));
    {
        let mut st = sh.state.lock().unwrap();
        if st.clients >= sh.cfg.max_clients {
            drop(st);
            refuse(&mut stream, err::SERVER_FULL, "server full");
            return;
        }
        st.clients += 1;
    }
    let joined = hello(&sh, &mut stream, &peer);
    if let Some((room, id, vip)) = joined {
        let _ = stream.set_read_timeout(Some(sh.cfg.keepalive));
        session(&sh, &mut stream, &room, id, vip);
        leave(&sh, &room, id, vip);
        log!(sh, "{peer}: member {id} ({}) left room '{room}'", proto::ip_str(vip));
    }
    let _ = stream.shutdown(Shutdown::Both);
    sh.state.lock().unwrap().clients -= 1;
}

/// Reads the first frames: `List` (answered, then wait for more) or `Hello`. Returns the room, member id and address once joined.
fn hello(sh: &Arc<Shared>, stream: &mut TcpStream, peer: &str) -> Option<(String, u32, u32)> {
    loop {
        let m = match proto::read_msg(stream) {
            Ok(m) => m,
            Err(ProtoError::Io(e)) if matches!(e.kind(), io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut) => {
                refuse(stream, err::TIMEOUT, "no hello");
                return None;
            }
            Err(ProtoError::Malformed(w)) => {
                refuse(stream, err::BAD_REQUEST, w);
                return None;
            }
            Err(_) => return None,
        };
        match m {
            Msg::List => {
                let st = sh.state.lock().unwrap();
                let mut rooms: Vec<RoomInfo> = st
                    .rooms
                    .iter()
                    .map(|(n, r)| RoomInfo {
                        name: n.clone(),
                        members: r.members.len() as u16,
                        max_members: sh.cfg.max_members as u16,
                        password: !r.password.is_empty(),
                    })
                    .collect();
                drop(st);
                rooms.sort_by(|a, b| a.name.cmp(&b.name));
                rooms.truncate(1024);
                send_now(stream, &Msg::ListResult { rooms });
            }
            Msg::Hello { version, want_vip, room, password, nick } => {
                if version != proto::VERSION {
                    refuse(stream, err::VERSION, &format!("server speaks protocol {}", proto::VERSION));
                    return None;
                }
                let room = if room.is_empty() { "lobby".to_string() } else { room };
                if !valid_room_name(&room) || password.len() > 64 {
                    refuse(stream, err::BAD_NAME, "room names are 1-64 letters, digits, spaces, '_', '.', '-'");
                    return None;
                }
                return join(sh, stream, peer, room, password, clean_nick(&nick), want_vip);
            }
            _ => {
                refuse(stream, err::BAD_REQUEST, "expected hello");
                return None;
            }
        }
    }
}

fn join(sh: &Arc<Shared>, stream: &mut TcpStream, peer: &str, room: String, password: String, nick: String, want_vip: u32) -> Option<(String, u32, u32)> {
    let (lo, hi) = host_range(sh.cfg.subnet);
    let mut st = sh.state.lock().unwrap();
    if !st.rooms.contains_key(&room) && st.rooms.len() >= sh.cfg.max_rooms {
        drop(st);
        refuse(stream, err::SERVER_FULL, "too many rooms");
        return None;
    }
    let r = st.rooms.entry(room.clone()).or_insert_with(|| Room { password: password.clone(), members: BTreeMap::new() });
    if !same_secret(&r.password, &password) {
        let empty = r.members.is_empty();
        if empty {
            st.rooms.remove(&room);
        }
        drop(st);
        refuse(stream, err::PASSWORD, "wrong room password");
        return None;
    }
    if r.members.len() >= sh.cfg.max_members {
        drop(st);
        refuse(stream, err::ROOM_FULL, "room full");
        return None;
    }
    let taken = |v: u32| r.members.values().any(|p| p.info.vip == v);
    let vip = if (lo..=hi).contains(&want_vip) && !taken(want_vip) { want_vip } else { (lo..=hi).find(|v| !taken(*v)).unwrap_or(0) };
    if vip == 0 {
        drop(st);
        refuse(stream, err::ROOM_FULL, "no free address");
        return None;
    }
    let (Ok(wstream), Ok(kill)) = (stream.try_clone(), stream.try_clone()) else { return None };
    let (tx, rx) = sync_channel::<Frame>(sh.cfg.queue);
    st.next_member = st.next_member.wrapping_add(1).max(1);
    let id = st.next_member;
    let r = st.rooms.get_mut(&room).unwrap();
    let info = Member { id, vip, nick };
    let others: Vec<Member> = r.members.values().map(|p| p.info.clone()).collect();
    let mut members = others.clone();
    members.push(info.clone());
    // the welcome goes first on the new member's queue, then everyone else hears of it
    let _ = tx.try_send(Arc::new(
        Msg::Welcome { member_id: id, vip, netmask: proto::netmask(sh.cfg.subnet.1), max_members: sh.cfg.max_members as u16, members }.encode(),
    ));
    let note: Frame = Arc::new(Msg::MemberJoin(info.clone()).encode());
    for p in r.members.values() {
        if p.tx.try_send(note.clone()).is_err() {
            let _ = p.kill.shutdown(Shutdown::Both);
        }
    }
    r.members.insert(id, Peer { info, tx, kill, ldn: None });
    drop(st);
    let _ = thread::Builder::new().name("napple-room-writer".into()).spawn(move || writer(wstream, rx));
    log!(sh, "{peer}: member {id} joined room '{room}' as {} ({} before it)", proto::ip_str(vip), others.len());
    Some((room, id, vip))
}

fn leave(sh: &Arc<Shared>, room: &str, id: u32, vip: u32) {
    let mut st = sh.state.lock().unwrap();
    let Some(r) = st.rooms.get_mut(room) else { return };
    r.members.remove(&id); // drops its queue: the writer ends
    let note: Frame = Arc::new(Msg::MemberLeave { id, vip }.encode());
    for p in r.members.values() {
        if p.tx.try_send(note.clone()).is_err() {
            let _ = p.kill.shutdown(Shutdown::Both);
        }
    }
    if r.members.is_empty() {
        st.rooms.remove(room);
    }
}

/// Sends a control frame to a member of `room`; a member whose queue is full is disconnected.
fn to_member(r: &Room, id: u32, f: Frame) {
    if let Some(p) = r.members.get(&id) {
        if p.tx.try_send(f).is_err() {
            let _ = p.kill.shutdown(Shutdown::Both);
        }
    }
}

fn session(sh: &Arc<Shared>, stream: &mut TcpStream, room: &str, id: u32, vip: u32) {
    let mut bytes = Bucket::new(sh.cfg.rate_bytes, sh.cfg.burst_bytes);
    let mut packets = Bucket::new(sh.cfg.rate_packets, sh.cfg.burst_packets);
    let bcast = broadcast_of(sh.cfg.subnet);
    loop {
        let m = match proto::read_msg(stream) {
            Ok(m) => m,
            Err(ProtoError::Malformed(w)) => {
                log!(sh, "member {id}: {w}; dropped");
                return;
            }
            Err(_) => return, // EOF, error or keepalive timeout
        };
        match m {
            Msg::Ping { token } => {
                let st = sh.state.lock().unwrap();
                if let Some(r) = st.rooms.get(room) {
                    to_member(r, id, Arc::new(Msg::Pong { token }.encode()));
                }
            }
            Msg::Packet { src, dst, body } => {
                if src != vip {
                    sh.stats.dropped_spoof.fetch_add(1, Ordering::Relaxed);
                    continue;
                }
                let size = body.len() as f64 + 16.0;
                if !packets.take(1.0) || !bytes.take(size) {
                    sh.stats.dropped_rate.fetch_add(1, Ordering::Relaxed);
                    continue;
                }
                let f: Frame = Arc::new(Msg::Packet { src, dst, body }.encode());
                let st = sh.state.lock().unwrap();
                let Some(r) = st.rooms.get(room) else { return };
                let mut sent = 0u64;
                let mut full = 0u64;
                if dst == u32::MAX || dst == bcast {
                    for p in r.members.values().filter(|p| p.info.id != id) {
                        match p.tx.try_send(f.clone()) {
                            Ok(()) => sent += 1,
                            Err(TrySendError::Full(_)) => full += 1,
                            Err(_) => {}
                        }
                    }
                } else if let Some(p) = r.members.values().find(|p| p.info.vip == dst) {
                    match p.tx.try_send(f) {
                        Ok(()) => sent += 1,
                        Err(TrySendError::Full(_)) => full += 1,
                        Err(_) => {}
                    }
                } else {
                    sh.stats.dropped_unroutable.fetch_add(1, Ordering::Relaxed);
                }
                sh.stats.relayed.fetch_add(sent, Ordering::Relaxed);
                sh.stats.dropped_queue.fetch_add(full, Ordering::Relaxed);
            }
            Msg::LdnAdvertise { lcid, blob } => {
                let mut st = sh.state.lock().unwrap();
                let Some(r) = st.rooms.get_mut(room) else { return };
                if let Some(p) = r.members.get_mut(&id) {
                    p.ldn = if blob.is_empty() { None } else { Some((lcid, blob)) };
                }
            }
            Msg::LdnScan { req, lcid } => {
                let st = sh.state.lock().unwrap();
                let Some(r) = st.rooms.get(room) else { return };
                let entries = r
                    .members
                    .values()
                    .filter(|p| p.info.id != id)
                    .filter_map(|p| p.ldn.as_ref().map(|(l, b)| (p, *l, b)))
                    .filter(|(_, l, _)| lcid == 0 || *l == lcid)
                    .map(|(p, l, b)| proto::LdnEntry { member: p.info.id, vip: p.info.vip, lcid: l, blob: b.clone() })
                    .collect();
                to_member(r, id, Arc::new(Msg::LdnScanResult { req, entries }.encode()));
            }
            Msg::Pong { .. } => {}
            _ => {
                log!(sh, "member {id}: unexpected frame; dropped");
                return;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn secrets_and_names() {
        assert!(same_secret("", ""));
        assert!(same_secret("abc", "abc"));
        assert!(!same_secret("abc", "abd"));
        assert!(!same_secret("abc", "abcd"));
        assert!(!same_secret("", "a"));
        assert!(valid_room_name("mk8d friday-1.0_x"));
        assert!(!valid_room_name(""));
        assert!(!valid_room_name("a/b"));
        assert_eq!(clean_nick("\u{7}"), "player");
        assert_eq!(host_range((0x0a0d0000, 16)), (0x0a0d0002, 0x0a0dfffe));
        assert_eq!(broadcast_of((0x0a0d0000, 16)), 0x0a0dffff);
    }
}
