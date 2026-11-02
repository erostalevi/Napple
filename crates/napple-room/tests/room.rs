//! The room server end to end, over real TCP on 127.0.0.1: room life cycle, passwords, address assignment, unicast and broadcast relay, LDN
//! advertise / scan / frame relay, anti-spoofing, limits (room size, rooms, rate) and keepalive.

use napple_room::client::{Client, JoinError, list};
use napple_room::proto::{Msg, body, err};
use napple_room::{Config, Handle, Server};
use std::io::Write;
use std::net::TcpStream;
use std::time::Duration;

const T: Duration = Duration::from_secs(3);
const SHORT: Duration = Duration::from_millis(300);

fn server(f: impl FnOnce(&mut Config)) -> Handle {
    let mut c = Config { listen: "127.0.0.1:0".parse().unwrap(), quiet: true, ..Config::default() };
    f(&mut c);
    Server::bind(c).unwrap().spawn().unwrap()
}

fn join(h: &Handle, room: &str, pw: &str, nick: &str) -> Client {
    Client::join(h.addr(), room, pw, nick, 0).unwrap()
}

fn refused(r: Result<Client, JoinError>) -> u32 {
    match r {
        Err(JoinError::Refused { code, .. }) => code,
        Err(e) => panic!("expected a refusal, got {e:?}"),
        Ok(_) => panic!("expected a refusal, got a welcome"),
    }
}

fn udp(src_port: u16, dst_port: u16, payload: &[u8]) -> Vec<u8> {
    let mut b = vec![body::UDP, 0];
    b.extend_from_slice(&src_port.to_le_bytes());
    b.extend_from_slice(&dst_port.to_le_bytes());
    b.extend_from_slice(payload);
    b
}

fn wait_census(h: &Handle, want: (usize, usize)) {
    for _ in 0..100 {
        if h.census() == want {
            return;
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    assert_eq!(h.census(), want);
}

#[test]
fn room_life_cycle_and_membership_notes() {
    let h = server(|_| {});
    let mut a = join(&h, "friday", "", "alice");
    assert_eq!(a.vip, 0x0a0d0002);
    assert_eq!(a.netmask, 0xffff0000);
    assert_eq!(a.members.len(), 1);
    let b = join(&h, "friday", "", "bob");
    assert_eq!(b.vip, 0x0a0d0003);
    assert_eq!(b.members.iter().map(|m| m.nick.as_str()).collect::<Vec<_>>(), ["alice", "bob"]);
    match a.recv(T) {
        Some(Msg::MemberJoin(m)) => assert_eq!((m.nick.as_str(), m.vip, m.id), ("bob", b.vip, b.member_id)),
        m => panic!("{m:?}"),
    }
    let rooms = list(h.addr()).unwrap();
    assert_eq!(rooms.len(), 1);
    assert_eq!((rooms[0].name.as_str(), rooms[0].members, rooms[0].password), ("friday", 2, false));
    let bid = b.member_id;
    let bvip = b.vip;
    drop(b);
    match a.recv(T) {
        Some(Msg::MemberLeave { id, vip }) => assert_eq!((id, vip), (bid, bvip)),
        m => panic!("{m:?}"),
    }
    // the freed address is given out again
    let c = join(&h, "friday", "", "carol");
    assert_eq!(c.vip, bvip);
    drop(a);
    drop(c);
    wait_census(&h, (0, 0));
    // a room of the same name made later is a new room (new password)
    let _d = join(&h, "friday", "new", "dave");
    assert_eq!(refused(Client::join(h.addr(), "friday", "", "eve", 0)), err::PASSWORD);
    h.shutdown();
}

#[test]
fn passwords_names_versions() {
    let h = server(|_| {});
    let _a = join(&h, "locked", "s3cret", "alice");
    assert_eq!(refused(Client::join(h.addr(), "locked", "wrong", "x", 0)), err::PASSWORD);
    assert_eq!(refused(Client::join(h.addr(), "locked", "", "x", 0)), err::PASSWORD);
    let _b = join(&h, "locked", "s3cret", "bob");
    assert!(list(h.addr()).unwrap()[0].password);
    assert_eq!(refused(Client::join(h.addr(), "bad/name", "", "x", 0)), err::BAD_NAME);
    // empty room name is the lobby
    let l = join(&h, "", "", "x");
    assert_eq!(l.members.len(), 1);
    assert!(list(h.addr()).unwrap().iter().any(|r| r.name == "lobby"));
    // a wrong protocol version is refused
    let mut s = TcpStream::connect(h.addr()).unwrap();
    s.write_all(&Msg::Hello { version: 99, want_vip: 0, room: "r".into(), password: "".into(), nick: "".into() }.encode()).unwrap();
    s.set_read_timeout(Some(T)).unwrap();
    match napple_room::proto::read_msg(&mut s).unwrap() {
        Msg::Error { code, .. } => assert_eq!(code, err::VERSION),
        m => panic!("{m:?}"),
    }
    // garbage instead of a hello: refused, connection closed
    let mut g = TcpStream::connect(h.addr()).unwrap();
    g.write_all(&[0xff; 16]).unwrap();
    g.set_read_timeout(Some(T)).unwrap();
    match napple_room::proto::read_msg(&mut g) {
        Ok(Msg::Error { code, .. }) => assert_eq!(code, err::BAD_REQUEST),
        r => panic!("{r:?}"),
    }
    h.shutdown();
}

#[test]
fn requested_addresses() {
    let h = server(|_| {});
    let a = Client::join(h.addr(), "r", "", "a", 0x0a0d0042).unwrap();
    assert_eq!(a.vip, 0x0a0d0042);
    let b = Client::join(h.addr(), "r", "", "b", 0x0a0d0042).unwrap(); // taken: another one
    assert_ne!(b.vip, 0x0a0d0042);
    let c = Client::join(h.addr(), "r", "", "c", 0x0b000005).unwrap(); // outside the subnet: another one
    assert_eq!(c.vip >> 16, 0x0a0d);
    let d = Client::join(h.addr(), "r", "", "d", 0x0a0dffff).unwrap(); // the broadcast address is never given
    assert_ne!(d.vip, 0x0a0dffff);
    h.shutdown();
}

#[test]
fn unicast_and_broadcast_relay() {
    let h = server(|_| {});
    let mut a = join(&h, "lan", "", "a");
    let mut b = join(&h, "lan", "", "b");
    let mut c = join(&h, "lan", "", "c");
    let mut other_room = join(&h, "elsewhere", "", "o");
    // unicast a -> b only
    a.send(&Msg::Packet { src: a.vip, dst: b.vip, body: udp(30000, 30000, b"hello b") }).unwrap();
    match b.recv_data(T) {
        Some(Msg::Packet { src, dst, body }) => assert_eq!((src, dst, body), (a.vip, b.vip, udp(30000, 30000, b"hello b"))),
        m => panic!("{m:?}"),
    }
    assert!(c.recv_data(SHORT).is_none());
    // subnet broadcast and limited broadcast reach everyone else in the room, not the sender, not other rooms
    for dst in [0x0a0dffffu32, u32::MAX] {
        a.send(&Msg::Packet { src: a.vip, dst, body: udp(49152, 49152, b"anyone?") }).unwrap();
        for m in [&mut b, &mut c] {
            match m.recv_data(T) {
                Some(Msg::Packet { src, dst: d, .. }) => assert_eq!((src, d), (a.vip, dst)),
                x => panic!("{x:?}"),
            }
        }
        assert!(a.recv_data(SHORT).is_none());
        assert!(other_room.recv_data(SHORT).is_none());
    }
    // a big datagram survives intact
    let big = vec![0x5au8; 60000];
    b.send(&Msg::Packet { src: b.vip, dst: a.vip, body: udp(1, 2, &big) }).unwrap();
    match a.recv_data(T) {
        Some(Msg::Packet { body, .. }) => assert_eq!(&body[6..], &big[..]),
        m => panic!("{m:?}"),
    }
    // nowhere to go: counted, nobody gets it
    a.send(&Msg::Packet { src: a.vip, dst: 0x0a0d0099, body: udp(1, 1, b"?") }).unwrap();
    assert!(b.recv_data(SHORT).is_none());
    assert_eq!(h.stats().dropped_unroutable.load(std::sync::atomic::Ordering::Relaxed), 1);
    h.shutdown();
}

#[test]
fn spoofed_sources_are_dropped() {
    let h = server(|_| {});
    let mut a = join(&h, "s", "", "a");
    let mut b = join(&h, "s", "", "b");
    let c = join(&h, "s", "", "c");
    a.send(&Msg::Packet { src: c.vip, dst: b.vip, body: udp(1, 1, b"i am c") }).unwrap();
    assert!(b.recv_data(SHORT).is_none());
    assert_eq!(h.stats().dropped_spoof.load(std::sync::atomic::Ordering::Relaxed), 1);
    h.shutdown();
}

#[test]
fn ldn_advertise_scan_and_frame_relay() {
    let h = server(|_| {});
    let mut host = join(&h, "ldn", "", "host");
    let mut sta = join(&h, "ldn", "", "station");
    let mut other = join(&h, "ldn", "", "other");
    let lcid = 0x0100152000022000u64;
    host.send(&Msg::LdnAdvertise { lcid, blob: b"network-info".to_vec() }).unwrap();
    other.send(&Msg::LdnAdvertise { lcid: 0x0100000000010000, blob: b"smo".to_vec() }).unwrap();
    std::thread::sleep(Duration::from_millis(50));
    // scan by title id: only the matching network, never one's own
    sta.send(&Msg::LdnScan { req: 7, lcid }).unwrap();
    match sta.recv_data(T) {
        Some(Msg::LdnScanResult { req, entries }) => {
            assert_eq!(req, 7);
            assert_eq!(entries.len(), 1);
            assert_eq!((entries[0].member, entries[0].vip, entries[0].lcid), (host.member_id, host.vip, lcid));
            assert_eq!(entries[0].blob, b"network-info");
        }
        m => panic!("{m:?}"),
    }
    sta.send(&Msg::LdnScan { req: 8, lcid: 0 }).unwrap();
    match sta.recv_data(T) {
        Some(Msg::LdnScanResult { entries, .. }) => assert_eq!(entries.len(), 2),
        m => panic!("{m:?}"),
    }
    host.send(&Msg::LdnScan { req: 9, lcid }).unwrap();
    match host.recv_data(T) {
        Some(Msg::LdnScanResult { entries, .. }) => assert!(entries.is_empty()),
        m => panic!("{m:?}"),
    }
    // the session frames (JOIN, ACCEPT, ...) travel as packets
    let mut join_frame = vec![body::LDN, 3, 0, 0, 0, 0];
    join_frame.extend_from_slice(b"JOIN");
    sta.send(&Msg::Packet { src: sta.vip, dst: host.vip, body: join_frame.clone() }).unwrap();
    match host.recv_data(T) {
        Some(Msg::Packet { src, body, .. }) => assert_eq!((src, body), (sta.vip, join_frame)),
        m => panic!("{m:?}"),
    }
    // withdrawn: no longer found
    host.send(&Msg::LdnAdvertise { lcid, blob: vec![] }).unwrap();
    std::thread::sleep(Duration::from_millis(50));
    sta.send(&Msg::LdnScan { req: 10, lcid }).unwrap();
    match sta.recv_data(T) {
        Some(Msg::LdnScanResult { entries, .. }) => assert!(entries.is_empty()),
        m => panic!("{m:?}"),
    }
    // a host that leaves takes its network with it
    other.send(&Msg::LdnAdvertise { lcid, blob: b"x".to_vec() }).unwrap();
    drop(other);
    std::thread::sleep(Duration::from_millis(100));
    sta.send(&Msg::LdnScan { req: 11, lcid }).unwrap();
    match sta.recv_data(T) {
        Some(Msg::LdnScanResult { entries, .. }) => assert!(entries.is_empty()),
        m => panic!("{m:?}"),
    }
    h.shutdown();
}

#[test]
fn room_and_server_limits() {
    let h = server(|c| {
        c.max_members = 2;
        c.max_rooms = 2;
        c.max_clients = 5;
    });
    let _a = join(&h, "one", "", "a");
    let _b = join(&h, "one", "", "b");
    assert_eq!(refused(Client::join(h.addr(), "one", "", "c", 0)), err::ROOM_FULL);
    let _c = join(&h, "two", "", "c");
    assert_eq!(refused(Client::join(h.addr(), "three", "", "d", 0)), err::SERVER_FULL);
    let _d = join(&h, "two", "", "d");
    // five connections: the sixth is turned away at the door
    let _e = TcpStream::connect(h.addr()).unwrap();
    std::thread::sleep(Duration::from_millis(100));
    assert_eq!(refused(Client::join(h.addr(), "two", "", "f", 0)), err::SERVER_FULL);
    h.shutdown();
}

#[test]
fn rate_limit_drops_excess_packets() {
    let h = server(|c| {
        c.rate_packets = 10;
        c.burst_packets = 20;
    });
    let mut a = join(&h, "r", "", "a");
    let mut b = join(&h, "r", "", "b");
    for i in 0..200u32 {
        a.send(&Msg::Packet { src: a.vip, dst: b.vip, body: udp(1, 1, &i.to_le_bytes()) }).unwrap();
    }
    let mut got = 0;
    while b.recv_data(SHORT).is_some() {
        got += 1;
    }
    assert!((20..=40).contains(&got), "got {got}");
    assert!(h.stats().dropped_rate.load(std::sync::atomic::Ordering::Relaxed) >= 150);
    // the member is still connected and keeps its share
    std::thread::sleep(Duration::from_millis(500));
    a.send(&Msg::Packet { src: a.vip, dst: b.vip, body: udp(1, 1, b"later") }).unwrap();
    assert!(b.recv_data(T).is_some());
    h.shutdown();
}

#[test]
fn keepalive_and_ping() {
    let h = server(|c| c.keepalive = Duration::from_millis(600));
    let mut a = join(&h, "k", "", "a");
    let mut b = join(&h, "k", "", "b");
    // a pings and stays; b is silent and is dropped
    for _ in 0..4 {
        a.send(&Msg::Ping { token: 5 }).unwrap();
        std::thread::sleep(Duration::from_millis(250));
    }
    let mut saw_pong = false;
    let mut saw_leave = false;
    while let Some(m) = a.recv(SHORT) {
        match m {
            Msg::Pong { token } => saw_pong |= token == 5,
            Msg::MemberLeave { id, .. } => saw_leave |= id == b.member_id,
            _ => {}
        }
    }
    assert!(saw_pong && saw_leave, "pong {saw_pong} leave {saw_leave}");
    assert!(b.recv(SHORT).is_none()); // closed
    wait_census(&h, (1, 1));
    h.shutdown();
}
