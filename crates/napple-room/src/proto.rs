//! The Napple room protocol, version 1: the frames a Napple runtime and a room server exchange over one TCP connection.
//!
//! Every frame is an 8-byte header, `u16 type, u16 reserved (0), u32 body length`, then the body; all integers little-endian, strings are a `u8` length
//! and that many bytes (UTF-8 expected, not required). The server routes `Packet` bodies without reading them; their layout belongs to the runtime
//! (`runtime/src/net/net_room.h`): `u8 proto, u8 kind, u16 src_port, u16 dst_port`, then the payload. Nothing in here is Nintendo's: the runtime puts the
//! LDN and LAN data the games already send inside these frames.

use std::io::{self, Read, Write};

pub const VERSION: u32 = 1;
pub const DEFAULT_PORT: u16 = 31415;
pub const HEADER: usize = 8;
/// The largest body a frame may carry (a UDP datagram of 65507 bytes plus the packet header, with room to spare).
pub const MAX_BODY: usize = 66 * 1024;

pub const T_HELLO: u16 = 1;
pub const T_WELCOME: u16 = 2;
pub const T_ERROR: u16 = 3;
pub const T_PING: u16 = 4;
pub const T_PONG: u16 = 5;
pub const T_MEMBER_JOIN: u16 = 6;
pub const T_MEMBER_LEAVE: u16 = 7;
pub const T_PACKET: u16 = 8;
pub const T_LDN_ADVERTISE: u16 = 9;
pub const T_LDN_SCAN: u16 = 10;
pub const T_LDN_SCAN_RESULT: u16 = 11;
pub const T_LIST: u16 = 12;
pub const T_LIST_RESULT: u16 = 13;

/// Error codes of an `Error` frame (the server closes the connection after sending one).
pub mod err {
    pub const VERSION: u32 = 1;
    pub const PASSWORD: u32 = 2;
    pub const ROOM_FULL: u32 = 3;
    pub const SERVER_FULL: u32 = 4;
    pub const BAD_REQUEST: u32 = 5;
    pub const BAD_NAME: u32 = 6;
    pub const TIMEOUT: u32 = 7;
}

/// Packet body protocols (first byte of a `Packet` body). The server does not look at them; listed here for tools and tests.
pub mod body {
    pub const UDP: u8 = 1;
    pub const TCP: u8 = 2;
    pub const LDN: u8 = 3;
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Member {
    pub id: u32,
    pub vip: u32,
    pub nick: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LdnEntry {
    pub member: u32,
    pub vip: u32,
    pub lcid: u64,
    pub blob: Vec<u8>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RoomInfo {
    pub name: String,
    pub members: u16,
    pub max_members: u16,
    pub password: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Msg {
    /// client -> server, first frame: join (or create) `room`. `want_vip` 0 lets the server choose the virtual address.
    Hello { version: u32, want_vip: u32, room: String, password: String, nick: String },
    /// server -> client: joined; the member list includes the new member itself.
    Welcome { member_id: u32, vip: u32, netmask: u32, max_members: u16, members: Vec<Member> },
    Error { code: u32, message: String },
    Ping { token: u64 },
    Pong { token: u64 },
    MemberJoin(Member),
    MemberLeave { id: u32, vip: u32 },
    /// Virtual-LAN or LDN data from `src` (must be the sender's own address) to `dst` (a member, or a broadcast address: every other member).
    Packet { src: u32, dst: u32, body: Vec<u8> },
    /// client -> server: the LDN network this member hosts (an empty blob withdraws it).
    LdnAdvertise { lcid: u64, blob: Vec<u8> },
    /// client -> server: the networks other members host (`lcid` 0 = every title).
    LdnScan { req: u32, lcid: u64 },
    LdnScanResult { req: u32, entries: Vec<LdnEntry> },
    /// client -> server, instead of `Hello`: the rooms on this server.
    List,
    ListResult { rooms: Vec<RoomInfo> },
}

#[derive(Debug)]
pub enum ProtoError {
    Io(io::Error),
    /// A frame that does not parse (bad length, truncated body, unknown type, reserved bits set).
    Malformed(&'static str),
}
impl From<io::Error> for ProtoError {
    fn from(e: io::Error) -> Self {
        ProtoError::Io(e)
    }
}
impl std::fmt::Display for ProtoError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ProtoError::Io(e) => write!(f, "{e}"),
            ProtoError::Malformed(m) => write!(f, "malformed frame: {m}"),
        }
    }
}

// ---- writing ------------------------------------------------------------------------------------------------------------------------------------------
struct W(Vec<u8>);
impl W {
    fn u8(&mut self, v: u8) {
        self.0.push(v);
    }
    fn u16(&mut self, v: u16) {
        self.0.extend_from_slice(&v.to_le_bytes());
    }
    fn u32(&mut self, v: u32) {
        self.0.extend_from_slice(&v.to_le_bytes());
    }
    fn u64(&mut self, v: u64) {
        self.0.extend_from_slice(&v.to_le_bytes());
    }
    fn str(&mut self, s: &str) {
        let b = s.as_bytes();
        let n = b.len().min(255);
        self.u8(n as u8);
        self.0.extend_from_slice(&b[..n]);
    }
    fn blob16(&mut self, b: &[u8]) {
        let n = b.len().min(u16::MAX as usize);
        self.u16(n as u16);
        self.0.extend_from_slice(&b[..n]);
    }
    fn member(&mut self, m: &Member) {
        self.u32(m.id);
        self.u32(m.vip);
        self.str(&m.nick);
    }
}

impl Msg {
    pub fn type_id(&self) -> u16 {
        match self {
            Msg::Hello { .. } => T_HELLO,
            Msg::Welcome { .. } => T_WELCOME,
            Msg::Error { .. } => T_ERROR,
            Msg::Ping { .. } => T_PING,
            Msg::Pong { .. } => T_PONG,
            Msg::MemberJoin(_) => T_MEMBER_JOIN,
            Msg::MemberLeave { .. } => T_MEMBER_LEAVE,
            Msg::Packet { .. } => T_PACKET,
            Msg::LdnAdvertise { .. } => T_LDN_ADVERTISE,
            Msg::LdnScan { .. } => T_LDN_SCAN,
            Msg::LdnScanResult { .. } => T_LDN_SCAN_RESULT,
            Msg::List => T_LIST,
            Msg::ListResult { .. } => T_LIST_RESULT,
        }
    }

    /// The whole frame, header included.
    pub fn encode(&self) -> Vec<u8> {
        let mut w = W(vec![0; HEADER]);
        match self {
            Msg::Hello { version, want_vip, room, password, nick } => {
                w.u32(*version);
                w.u32(*want_vip);
                w.str(room);
                w.str(password);
                w.str(nick);
            }
            Msg::Welcome { member_id, vip, netmask, max_members, members } => {
                w.u32(*member_id);
                w.u32(*vip);
                w.u32(*netmask);
                w.u16(*max_members);
                w.u16(members.len() as u16);
                members.iter().for_each(|m| w.member(m));
            }
            Msg::Error { code, message } => {
                w.u32(*code);
                w.str(message);
            }
            Msg::Ping { token } | Msg::Pong { token } => w.u64(*token),
            Msg::MemberJoin(m) => w.member(m),
            Msg::MemberLeave { id, vip } => {
                w.u32(*id);
                w.u32(*vip);
            }
            Msg::Packet { src, dst, body } => {
                w.u32(*src);
                w.u32(*dst);
                w.0.extend_from_slice(body);
            }
            Msg::LdnAdvertise { lcid, blob } => {
                w.u64(*lcid);
                w.blob16(blob);
            }
            Msg::LdnScan { req, lcid } => {
                w.u32(*req);
                w.u64(*lcid);
            }
            Msg::LdnScanResult { req, entries } => {
                w.u32(*req);
                w.u16(entries.len() as u16);
                for e in entries {
                    w.u32(e.member);
                    w.u32(e.vip);
                    w.u64(e.lcid);
                    w.blob16(&e.blob);
                }
            }
            Msg::List => {}
            Msg::ListResult { rooms } => {
                w.u16(rooms.len() as u16);
                for r in rooms {
                    w.str(&r.name);
                    w.u16(r.members);
                    w.u16(r.max_members);
                    w.u8(r.password as u8);
                }
            }
        }
        let len = (w.0.len() - HEADER) as u32;
        w.0[0..2].copy_from_slice(&self.type_id().to_le_bytes());
        w.0[4..8].copy_from_slice(&len.to_le_bytes());
        w.0
    }

    /// Parses one frame body of type `t`. Trailing bytes are an error (except in `Packet`, whose body is the rest).
    pub fn decode(t: u16, body: &[u8]) -> Result<Msg, ProtoError> {
        let mut r = R { b: body, off: 0 };
        let m = match t {
            T_HELLO => Msg::Hello { version: r.u32()?, want_vip: r.u32()?, room: r.str()?, password: r.str()?, nick: r.str()? },
            T_WELCOME => {
                let (member_id, vip, netmask, max_members) = (r.u32()?, r.u32()?, r.u32()?, r.u16()?);
                let n = r.u16()?;
                let mut members = Vec::with_capacity(n.min(256) as usize);
                for _ in 0..n {
                    members.push(r.member()?);
                }
                Msg::Welcome { member_id, vip, netmask, max_members, members }
            }
            T_ERROR => Msg::Error { code: r.u32()?, message: r.str()? },
            T_PING => Msg::Ping { token: r.u64()? },
            T_PONG => Msg::Pong { token: r.u64()? },
            T_MEMBER_JOIN => Msg::MemberJoin(r.member()?),
            T_MEMBER_LEAVE => Msg::MemberLeave { id: r.u32()?, vip: r.u32()? },
            T_PACKET => {
                let (src, dst) = (r.u32()?, r.u32()?);
                let body = r.rest().to_vec();
                Msg::Packet { src, dst, body }
            }
            T_LDN_ADVERTISE => Msg::LdnAdvertise { lcid: r.u64()?, blob: r.blob16()? },
            T_LDN_SCAN => Msg::LdnScan { req: r.u32()?, lcid: r.u64()? },
            T_LDN_SCAN_RESULT => {
                let req = r.u32()?;
                let n = r.u16()?;
                let mut entries = Vec::with_capacity(n.min(256) as usize);
                for _ in 0..n {
                    entries.push(LdnEntry { member: r.u32()?, vip: r.u32()?, lcid: r.u64()?, blob: r.blob16()? });
                }
                Msg::LdnScanResult { req, entries }
            }
            T_LIST => Msg::List,
            T_LIST_RESULT => {
                let n = r.u16()?;
                let mut rooms = Vec::with_capacity(n.min(1024) as usize);
                for _ in 0..n {
                    rooms.push(RoomInfo { name: r.str()?, members: r.u16()?, max_members: r.u16()?, password: r.u8()? != 0 });
                }
                Msg::ListResult { rooms }
            }
            _ => return Err(ProtoError::Malformed("unknown frame type")),
        };
        if r.off != body.len() {
            return Err(ProtoError::Malformed("trailing bytes"));
        }
        Ok(m)
    }
}

// ---- reading ------------------------------------------------------------------------------------------------------------------------------------------
struct R<'a> {
    b: &'a [u8],
    off: usize,
}
impl<'a> R<'a> {
    fn take(&mut self, n: usize) -> Result<&'a [u8], ProtoError> {
        if self.off + n > self.b.len() {
            return Err(ProtoError::Malformed("truncated"));
        }
        let s = &self.b[self.off..self.off + n];
        self.off += n;
        Ok(s)
    }
    fn u8(&mut self) -> Result<u8, ProtoError> {
        Ok(self.take(1)?[0])
    }
    fn u16(&mut self) -> Result<u16, ProtoError> {
        Ok(u16::from_le_bytes(self.take(2)?.try_into().unwrap()))
    }
    fn u32(&mut self) -> Result<u32, ProtoError> {
        Ok(u32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }
    fn u64(&mut self) -> Result<u64, ProtoError> {
        Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap()))
    }
    fn str(&mut self) -> Result<String, ProtoError> {
        let n = self.u8()? as usize;
        Ok(String::from_utf8_lossy(self.take(n)?).into_owned())
    }
    fn blob16(&mut self) -> Result<Vec<u8>, ProtoError> {
        let n = self.u16()? as usize;
        Ok(self.take(n)?.to_vec())
    }
    fn member(&mut self) -> Result<Member, ProtoError> {
        Ok(Member { id: self.u32()?, vip: self.u32()?, nick: self.str()? })
    }
    fn rest(&mut self) -> &'a [u8] {
        let s = &self.b[self.off..];
        self.off = self.b.len();
        s
    }
}

/// Reads one frame: its type and body. EOF before a header is `UnexpectedEof`.
pub fn read_frame(r: &mut impl Read) -> Result<(u16, Vec<u8>), ProtoError> {
    let mut h = [0u8; HEADER];
    r.read_exact(&mut h)?;
    let t = u16::from_le_bytes([h[0], h[1]]);
    let reserved = u16::from_le_bytes([h[2], h[3]]);
    let len = u32::from_le_bytes([h[4], h[5], h[6], h[7]]) as usize;
    if reserved != 0 {
        return Err(ProtoError::Malformed("reserved bits set"));
    }
    if len > MAX_BODY {
        return Err(ProtoError::Malformed("frame too large"));
    }
    let mut body = vec![0u8; len];
    r.read_exact(&mut body)?;
    Ok((t, body))
}

pub fn read_msg(r: &mut impl Read) -> Result<Msg, ProtoError> {
    let (t, body) = read_frame(r)?;
    Msg::decode(t, &body)
}

pub fn write_msg(w: &mut impl Write, m: &Msg) -> io::Result<()> {
    w.write_all(&m.encode())
}

// ---- addresses ----------------------------------------------------------------------------------------------------------------------------------------
/// Parses `a.b.c.d/len` into (network, prefix length). The network bits below the prefix are cleared.
pub fn parse_cidr(s: &str) -> Option<(u32, u8)> {
    let (ip, len) = s.split_once('/')?;
    let ip: std::net::Ipv4Addr = ip.trim().parse().ok()?;
    let len: u8 = len.trim().parse().ok()?;
    if !(8..=30).contains(&len) {
        return None;
    }
    let mask = netmask(len);
    Some((u32::from(ip) & mask, len))
}
pub fn netmask(prefix: u8) -> u32 {
    if prefix == 0 { 0 } else { u32::MAX << (32 - prefix as u32) }
}
pub fn ip_str(v: u32) -> String {
    std::net::Ipv4Addr::from(v).to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn round(m: Msg) {
        let f = m.encode();
        let (t, body) = read_frame(&mut &f[..]).unwrap();
        assert_eq!(Msg::decode(t, &body).unwrap(), m);
    }

    #[test]
    fn every_message_round_trips() {
        let mem = Member { id: 7, vip: 0x0a0d0002, nick: "neto".into() };
        round(Msg::Hello { version: VERSION, want_vip: 0, room: "friday".into(), password: "pw".into(), nick: "a".into() });
        round(Msg::Welcome { member_id: 1, vip: 2, netmask: 0xffff0000, max_members: 8, members: vec![mem.clone()] });
        round(Msg::Error { code: err::PASSWORD, message: "wrong password".into() });
        round(Msg::Ping { token: 99 });
        round(Msg::Pong { token: 99 });
        round(Msg::MemberJoin(mem));
        round(Msg::MemberLeave { id: 3, vip: 4 });
        round(Msg::Packet { src: 1, dst: 2, body: vec![1, 2, 3] });
        round(Msg::LdnAdvertise { lcid: 0x0100152000022000, blob: vec![9; 300] });
        round(Msg::LdnScan { req: 5, lcid: 0 });
        round(Msg::LdnScanResult { req: 5, entries: vec![LdnEntry { member: 1, vip: 2, lcid: 3, blob: vec![4] }] });
        round(Msg::List);
        round(Msg::ListResult { rooms: vec![RoomInfo { name: "x".into(), members: 1, max_members: 8, password: true }] });
    }

    #[test]
    fn hostile_frames_are_rejected() {
        let mut f = Msg::Ping { token: 1 }.encode();
        f[2] = 1;
        assert!(matches!(read_frame(&mut &f[..]), Err(ProtoError::Malformed(_))));
        let mut big = vec![T_PACKET as u8, 0, 0, 0];
        big.extend_from_slice(&((MAX_BODY + 1) as u32).to_le_bytes());
        assert!(matches!(read_frame(&mut &big[..]), Err(ProtoError::Malformed(_))));
        assert!(Msg::decode(T_HELLO, &[1, 0, 0]).is_err());
        assert!(Msg::decode(T_PING, &[0; 9]).is_err());
        assert!(Msg::decode(999, &[]).is_err());
        // a member count far beyond the body does not allocate or panic
        assert!(Msg::decode(T_WELCOME, &[0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0xff, 0xff]).is_err());
    }

    #[test]
    fn cidr() {
        assert_eq!(parse_cidr("10.13.0.0/16"), Some((0x0a0d0000, 16)));
        assert_eq!(parse_cidr("10.13.4.5/16"), Some((0x0a0d0000, 16)));
        assert_eq!(parse_cidr("10.13.0.0/31"), None);
        assert_eq!(netmask(16), 0xffff0000);
    }
}
