//! PRUDP "Lite", the packet format Switch NEX titles use inside their WebSocket connection, and the server side of a PRUDP connection: the
//! SYN / CONNECT handshake, reliable ordered delivery with acknowledgements (single and aggregate), fragmentation, retransmission, ping and
//! disconnect.
//!
//! Lite header (12 bytes, little endian):
//!   0 magic 0x80 | 1 options length | 2 payload size (u16) | 4 stream types (source << 4 | destination) | 5 source port | 6 destination port
//!   | 7 fragment id | 8 type | flags << 4 (u16) | 10 sequence id (u16) | options (id, size, value)... | payload
//! The TLS layer underneath already protects the stream, so Lite payloads are neither signed per packet nor encrypted.

use std::collections::BTreeMap;

pub const MAGIC_LITE: u8 = 0x80;

pub const TYPE_SYN: u8 = 0;
pub const TYPE_CONNECT: u8 = 1;
pub const TYPE_DATA: u8 = 2;
pub const TYPE_DISCONNECT: u8 = 3;
pub const TYPE_PING: u8 = 4;

pub const FLAG_ACK: u16 = 0x001;
pub const FLAG_RELIABLE: u16 = 0x002;
pub const FLAG_NEED_ACK: u16 = 0x004;
pub const FLAG_HAS_SIZE: u16 = 0x008;
pub const FLAG_MULTI_ACK: u16 = 0x200;

pub const OPT_SUPPORT: u8 = 0;
pub const OPT_CONNECTION_SIG: u8 = 1;
pub const OPT_FRAGMENT: u8 = 2;
pub const OPT_LITE_SIG: u8 = 0x80;

/// RVSecure, the stream type of every NEX connection.
pub const STREAM_RVSECURE: u8 = 10;
/// Largest payload of one fragment.
pub const MAX_FRAGMENT: usize = 1300;
/// Retransmission: first resend after this long, then doubling, up to MAX_TRIES sends.
pub const RESEND_MS: u64 = 1500;
pub const MAX_TRIES: u32 = 6;

#[derive(Debug, Clone, PartialEq, Default)]
pub struct Packet {
    pub src_type: u8,
    pub dst_type: u8,
    pub src_port: u8,
    pub dst_port: u8,
    pub fragment: u8,
    pub ptype: u8,
    pub flags: u16,
    pub seq: u16,
    pub options: Vec<(u8, Vec<u8>)>,
    pub payload: Vec<u8>,
}

impl Packet {
    pub fn has(&self, flag: u16) -> bool {
        self.flags & flag != 0
    }
    pub fn option(&self, id: u8) -> Option<&[u8]> {
        self.options.iter().find(|(i, _)| *i == id).map(|(_, v)| v.as_slice())
    }

    pub fn encode(&self) -> Vec<u8> {
        let opts: Vec<u8> = self.options.iter().flat_map(|(id, v)| [vec![*id, v.len() as u8], v.clone()].concat()).collect();
        let mut out = Vec::with_capacity(12 + opts.len() + self.payload.len());
        out.push(MAGIC_LITE);
        out.push(opts.len() as u8);
        out.extend_from_slice(&(self.payload.len() as u16).to_le_bytes());
        out.push(self.src_type << 4 | (self.dst_type & 15));
        out.push(self.src_port);
        out.push(self.dst_port);
        out.push(self.fragment);
        out.extend_from_slice(&((self.flags << 4) | self.ptype as u16).to_le_bytes());
        out.extend_from_slice(&self.seq.to_le_bytes());
        out.extend_from_slice(&opts);
        out.extend_from_slice(&self.payload);
        out
    }

    /// Decodes one packet; returns it and the bytes it used.
    pub fn decode(data: &[u8]) -> Option<(Packet, usize)> {
        if data.len() < 12 || data[0] != MAGIC_LITE {
            return None;
        }
        let optlen = data[1] as usize;
        let size = u16::from_le_bytes([data[2], data[3]]) as usize;
        let total = 12 + optlen + size;
        if data.len() < total {
            return None;
        }
        let tf = u16::from_le_bytes([data[8], data[9]]);
        let mut options = Vec::new();
        let mut o = &data[12..12 + optlen];
        while o.len() >= 2 {
            let n = o[1] as usize;
            if o.len() < 2 + n {
                return None;
            }
            options.push((o[0], o[2..2 + n].to_vec()));
            o = &o[2 + n..];
        }
        let p = Packet {
            src_type: data[4] >> 4,
            dst_type: data[4] & 15,
            src_port: data[5],
            dst_port: data[6],
            fragment: data[7],
            ptype: (tf & 15) as u8,
            flags: tf >> 4,
            seq: u16::from_le_bytes([data[10], data[11]]),
            options,
            payload: data[12 + optlen..total].to_vec(),
        };
        Some((p, total))
    }
}

/// Every packet in a buffer (a WebSocket message may carry several). Stops at the first malformed one.
pub fn decode_all(mut data: &[u8]) -> Vec<Packet> {
    let mut v = Vec::new();
    while let Some((p, n)) = Packet::decode(data) {
        v.push(p);
        data = &data[n..];
    }
    v
}

fn seq_le(a: u16, b: u16) -> bool {
    (b.wrapping_sub(a) as i16) >= 0
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum State {
    Opening,
    Connected,
    Closed,
}

/// What a received packet produced.
#[derive(Debug, Default, PartialEq)]
pub struct Events {
    /// Packets to send back now.
    pub out: Vec<Packet>,
    /// Complete reliable messages, in order.
    pub messages: Vec<Vec<u8>>,
    /// Unreliable DATA payloads.
    pub unreliable: Vec<Vec<u8>>,
    pub connected: bool,
    pub closed: bool,
}

struct Pending {
    packet: Packet,
    sent_ms: u64,
    tries: u32,
}

/// The server side of one PRUDP connection (one client port on one server port).
pub struct Connection {
    pub local_port: u8,
    pub remote_port: u8,
    pub state: State,
    pub signature: [u8; 16],
    pub support: Vec<u8>,
    next_out: u16,
    expected_in: u16,
    got_data: bool,
    out_of_order: BTreeMap<u16, Packet>,
    fragments: Vec<u8>,
    unacked: BTreeMap<u16, Pending>,
    pub last_heard_ms: u64,
}

impl Connection {
    pub fn new(local_port: u8, remote_port: u8, signature: [u8; 16]) -> Connection {
        Connection {
            local_port,
            remote_port,
            state: State::Opening,
            signature,
            support: vec![],
            next_out: 1,
            expected_in: 1,
            got_data: false,
            out_of_order: BTreeMap::new(),
            fragments: vec![],
            unacked: BTreeMap::new(),
            last_heard_ms: 0,
        }
    }

    fn reply(&self, ptype: u8, flags: u16, seq: u16) -> Packet {
        Packet {
            src_type: STREAM_RVSECURE,
            dst_type: STREAM_RVSECURE,
            src_port: self.local_port,
            dst_port: self.remote_port,
            ptype,
            flags,
            seq,
            ..Default::default()
        }
    }

    pub fn unacked_count(&self) -> usize {
        self.unacked.len()
    }

    /// Handles one packet from the client. `on_connect` sees a CONNECT payload and returns the CONNECT-ACK payload, or None to refuse the
    /// connection (the packet is then ignored and the connection closed).
    pub fn receive(&mut self, p: &Packet, now_ms: u64, on_connect: &mut dyn FnMut(&[u8]) -> Option<Vec<u8>>) -> Events {
        let mut ev = Events::default();
        self.last_heard_ms = now_ms;
        if p.has(FLAG_ACK) || p.has(FLAG_MULTI_ACK) {
            if p.ptype == TYPE_DATA || p.ptype == TYPE_DISCONNECT || p.ptype == TYPE_PING {
                self.acknowledged(p);
            }
            return ev;
        }
        match p.ptype {
            TYPE_SYN => {
                // a repeated SYN gets the same answer
                self.support = p.option(OPT_SUPPORT).map(|s| s.to_vec()).unwrap_or_else(|| 0u32.to_le_bytes().to_vec());
                let mut r = self.reply(TYPE_SYN, FLAG_ACK, p.seq);
                r.options = vec![(OPT_SUPPORT, self.support.clone()), (OPT_CONNECTION_SIG, self.signature.to_vec())];
                ev.out.push(r);
            }
            TYPE_CONNECT => {
                if self.state == State::Connected {
                    // our CONNECT-ACK was lost: it is not repeated here (the payload check already ran); the client times out and retries a SYN
                    return ev;
                }
                match on_connect(&p.payload) {
                    Some(resp) => {
                        if let Some(s) = p.option(OPT_SUPPORT) {
                            self.support = s.to_vec();
                        }
                        let mut r = self.reply(TYPE_CONNECT, FLAG_ACK, p.seq);
                        r.options = vec![(OPT_SUPPORT, self.support.clone())];
                        r.payload = resp;
                        ev.out.push(r);
                        self.state = State::Connected;
                        self.expected_in = p.seq.wrapping_add(1);
                        ev.connected = true;
                    }
                    None => {
                        self.state = State::Closed;
                        ev.closed = true;
                    }
                }
            }
            TYPE_DATA => {
                if self.state != State::Connected {
                    return ev;
                }
                if !p.has(FLAG_RELIABLE) {
                    if p.has(FLAG_NEED_ACK) {
                        ev.out.push(self.reply(TYPE_DATA, FLAG_ACK, p.seq));
                    }
                    ev.unreliable.push(p.payload.clone());
                    return ev;
                }
                if p.has(FLAG_NEED_ACK) {
                    ev.out.push(self.reply(TYPE_DATA, FLAG_ACK, p.seq));
                }
                if !self.got_data {
                    // the first reliable DATA fixes where the client's sequence starts (CONNECT + 1 in practice; the stream underneath is in order)
                    self.got_data = true;
                    self.expected_in = p.seq;
                }
                if seq_le(p.seq, self.expected_in.wrapping_sub(1)) {
                    return ev; // a duplicate (our ack was lost): acked again above
                }
                self.out_of_order.insert(p.seq, p.clone());
                while let Some(q) = self.out_of_order.remove(&self.expected_in) {
                    self.expected_in = self.expected_in.wrapping_add(1);
                    self.fragments.extend_from_slice(&q.payload);
                    if q.fragment == 0 {
                        ev.messages.push(std::mem::take(&mut self.fragments));
                    }
                }
            }
            TYPE_PING => {
                if p.has(FLAG_NEED_ACK) {
                    ev.out.push(self.reply(TYPE_PING, FLAG_ACK, p.seq));
                }
            }
            TYPE_DISCONNECT => {
                let a = self.reply(TYPE_DISCONNECT, FLAG_ACK, p.seq);
                ev.out.extend([a.clone(), a.clone(), a]);
                self.state = State::Closed;
                ev.closed = true;
            }
            _ => {}
        }
        ev
    }

    fn acknowledged(&mut self, p: &Packet) {
        if p.has(FLAG_MULTI_ACK) {
            // aggregate ack (new format): u8 substream, u8 count, u16 everything up to here, u16 * count further ids
            let d = &p.payload;
            if d.len() >= 4 {
                let count = d[1] as usize;
                let base = u16::from_le_bytes([d[2], d[3]]);
                self.unacked.retain(|s, _| !seq_le(*s, base));
                for i in 0..count {
                    if d.len() >= 6 + 2 * i {
                        let s = u16::from_le_bytes([d[4 + 2 * i], d[5 + 2 * i]]);
                        self.unacked.remove(&s);
                    }
                }
            }
        } else {
            self.unacked.remove(&p.seq);
        }
    }

    /// Queues a reliable message (split into fragments) and returns the packets to send.
    pub fn send(&mut self, message: &[u8], now_ms: u64) -> Vec<Packet> {
        let chunks: Vec<&[u8]> = if message.is_empty() { vec![&[][..]] } else { message.chunks(MAX_FRAGMENT).collect() };
        let n = chunks.len();
        let mut out = Vec::with_capacity(n);
        for (i, c) in chunks.into_iter().enumerate() {
            let mut p = self.reply(TYPE_DATA, FLAG_RELIABLE | FLAG_NEED_ACK, self.next_out);
            p.fragment = if i + 1 == n { 0 } else { (i + 1) as u8 };
            p.payload = c.to_vec();
            self.unacked.insert(self.next_out, Pending { packet: p.clone(), sent_ms: now_ms, tries: 1 });
            self.next_out = self.next_out.wrapping_add(1);
            out.push(p);
        }
        out
    }

    /// Packets whose acknowledgement is overdue. Err(()) when one has been sent MAX_TRIES times: the client is gone.
    pub fn resend_due(&mut self, now_ms: u64) -> Result<Vec<Packet>, ()> {
        let mut out = Vec::new();
        for pend in self.unacked.values_mut() {
            let wait = RESEND_MS << (pend.tries - 1).min(4);
            if now_ms >= pend.sent_ms + wait {
                if pend.tries >= MAX_TRIES {
                    self.state = State::Closed;
                    return Err(());
                }
                pend.tries += 1;
                pend.sent_ms = now_ms;
                out.push(pend.packet.clone());
            }
        }
        Ok(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn client(ptype: u8, flags: u16, seq: u16) -> Packet {
        Packet { src_type: 10, dst_type: 10, src_port: 0x1f, dst_port: 1, ptype, flags, seq, ..Default::default() }
    }

    #[test]
    fn codec_roundtrip() {
        let mut p = client(TYPE_SYN, FLAG_NEED_ACK, 0);
        p.options = vec![(OPT_SUPPORT, vec![4, 0, 0, 0])];
        let e = p.encode();
        assert_eq!(&e[..12], &[0x80, 6, 0, 0, 0xaa, 0x1f, 1, 0, 0x40, 0, 0, 0]);
        assert_eq!(Packet::decode(&e), Some((p.clone(), e.len())));
        let mut d = client(TYPE_DATA, FLAG_RELIABLE | FLAG_NEED_ACK, 7);
        d.fragment = 2;
        d.payload = vec![1, 2, 3];
        let mut two = d.encode();
        two.extend(p.encode());
        assert_eq!(decode_all(&two), vec![d, p]);
        assert!(Packet::decode(&e[..e.len() - 1]).is_none());
        assert!(Packet::decode(&[0xea, 0xd0, 1]).is_none());
    }

    fn connected(on: &mut dyn FnMut(&[u8]) -> Option<Vec<u8>>) -> Connection {
        let mut c = Connection::new(1, 0x1f, [7; 16]);
        let mut syn = client(TYPE_SYN, FLAG_NEED_ACK, 0);
        syn.options = vec![(OPT_SUPPORT, vec![4, 0, 0, 0])];
        let ev = c.receive(&syn, 0, on);
        assert_eq!(ev.out.len(), 1);
        let sa = &ev.out[0];
        assert_eq!((sa.ptype, sa.flags, sa.src_port, sa.dst_port), (TYPE_SYN, FLAG_ACK, 1, 0x1f));
        assert_eq!(sa.option(OPT_CONNECTION_SIG), Some(&[7u8; 16][..]));
        assert_eq!(sa.option(OPT_SUPPORT), Some(&[4u8, 0, 0, 0][..]));
        let mut con = client(TYPE_CONNECT, FLAG_RELIABLE | FLAG_NEED_ACK, 1);
        con.options = vec![(OPT_SUPPORT, vec![4, 0, 0, 0]), (OPT_LITE_SIG, vec![0; 16])];
        con.payload = b"ticket".to_vec();
        let ev = c.receive(&con, 0, on);
        assert!(ev.connected);
        assert_eq!(ev.out[0].ptype, TYPE_CONNECT);
        assert_eq!(ev.out[0].flags, FLAG_ACK);
        assert_eq!(ev.out[0].seq, 1);
        c
    }

    #[test]
    fn handshake_and_refusal() {
        let mut seen = vec![];
        let c = connected(&mut |p| {
            seen.push(p.to_vec());
            Some(b"ok".to_vec())
        });
        assert_eq!(seen, vec![b"ticket".to_vec()]);
        assert_eq!(c.state, State::Connected);
        let mut c = Connection::new(2, 0x1f, [0; 16]);
        let ev = c.receive(&client(TYPE_CONNECT, FLAG_RELIABLE | FLAG_NEED_ACK, 1), 0, &mut |_| None);
        assert!(ev.closed && ev.out.is_empty());
    }

    #[test]
    fn reliable_in_order_with_fragments() {
        let mut c = connected(&mut |_| Some(vec![]));
        let mk = |seq: u16, frag: u8, data: &[u8]| {
            let mut p = client(TYPE_DATA, FLAG_RELIABLE | FLAG_NEED_ACK, seq);
            p.fragment = frag;
            p.payload = data.to_vec();
            p
        };
        // the first fragment of a message, then seq 4 before seq 3: acked, held back
        let ev = c.receive(&mk(2, 1, b"AB"), 1, &mut |_| None);
        assert_eq!((ev.out[0].flags, ev.out[0].seq), (FLAG_ACK, 2));
        assert!(ev.messages.is_empty());
        let ev = c.receive(&mk(4, 0, b"D"), 2, &mut |_| None);
        assert_eq!(ev.out[0].seq, 4);
        assert!(ev.messages.is_empty());
        let ev = c.receive(&mk(3, 0, b"C"), 3, &mut |_| None);
        assert_eq!(ev.messages, vec![b"ABC".to_vec(), b"D".to_vec()]);
        // a duplicate is acked again but not delivered twice
        let ev = c.receive(&mk(3, 0, b"C"), 4, &mut |_| None);
        assert_eq!(ev.out.len(), 1);
        assert!(ev.messages.is_empty());
        let ev = c.receive(&mk(5, 0, b"E"), 5, &mut |_| None);
        assert_eq!(ev.messages, vec![b"E".to_vec()]);
    }

    #[test]
    fn send_fragments_ack_and_retransmit() {
        let mut c = connected(&mut |_| Some(vec![]));
        let big = vec![0x5a; MAX_FRAGMENT * 2 + 10];
        let out = c.send(&big, 100);
        assert_eq!(out.iter().map(|p| (p.seq, p.fragment, p.payload.len())).collect::<Vec<_>>(), vec![(1, 1, 1300), (2, 2, 1300), (3, 0, 10)]);
        assert!(out.iter().all(|p| p.flags == FLAG_RELIABLE | FLAG_NEED_ACK && p.dst_port == 0x1f));
        assert_eq!(c.unacked_count(), 3);
        // nothing due yet; then everything unacked is resent
        assert!(c.resend_due(100 + RESEND_MS - 1).unwrap().is_empty());
        // a single ack for 2
        c.receive(&client(TYPE_DATA, FLAG_ACK, 2), 200, &mut |_| None);
        assert_eq!(c.unacked_count(), 2);
        let again = c.resend_due(100 + RESEND_MS).unwrap();
        assert_eq!(again.iter().map(|p| p.seq).collect::<Vec<_>>(), vec![1, 3]);
        // backoff doubles
        assert!(c.resend_due(100 + RESEND_MS + RESEND_MS * 2 - 1).unwrap().is_empty());
        assert_eq!(c.resend_due(100 + RESEND_MS + RESEND_MS * 2).unwrap().len(), 2);
        // aggregate ack: everything up to 1, plus 3
        let mut agg = client(TYPE_DATA, FLAG_MULTI_ACK, 0);
        agg.payload = vec![0, 1, 1, 0, 3, 0];
        c.receive(&agg, 300, &mut |_| None);
        assert_eq!(c.unacked_count(), 0);
        // a client that never acks is dropped after MAX_TRIES sends
        c.send(b"x", 1000);
        let mut t = 1000;
        let mut dropped = false;
        for _ in 0..20 {
            t += RESEND_MS * 16;
            if c.resend_due(t).is_err() {
                dropped = true;
                break;
            }
        }
        assert!(dropped);
        assert_eq!(c.state, State::Closed);
    }

    #[test]
    fn ping_and_disconnect() {
        let mut c = connected(&mut |_| Some(vec![]));
        let ev = c.receive(&client(TYPE_PING, FLAG_NEED_ACK, 9), 5, &mut |_| None);
        assert_eq!((ev.out[0].ptype, ev.out[0].flags, ev.out[0].seq), (TYPE_PING, FLAG_ACK, 9));
        let ev = c.receive(&client(TYPE_DISCONNECT, FLAG_RELIABLE | FLAG_NEED_ACK, 5), 6, &mut |_| None);
        assert!(ev.closed);
        assert_eq!(ev.out.len(), 3);
        assert!(ev.out.iter().all(|p| p.ptype == TYPE_DISCONNECT && p.flags == FLAG_ACK));
    }
}
