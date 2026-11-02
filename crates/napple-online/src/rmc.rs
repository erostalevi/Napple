//! RMC (remote method call) messages carried in PRUDP DATA payloads.
//!
//! Request:  u32 size | u8 protocol | 0x80 [u16 extended protocol when the id is 0x7f] | u32 call id | u32 method | parameters
//! Response: u32 size | u8 protocol [u16 extended] | u8 1 | u32 call id | u32 method | 0x8000 | data
//!           u32 size | u8 protocol [u16 extended] | u8 0 | u32 error code | u32 call id
//! The size counts everything after itself. The server also sends requests (notifications) to clients.

use crate::nex::{DecodeError, Reader, Result, Writer};

/// NEX result codes the server returns (error bit 0x80000000 | category << 16 | code).
pub mod err {
    pub const CORE_UNKNOWN: u32 = 0x8001_0001;
    pub const CORE_NOT_IMPLEMENTED: u32 = 0x8001_0002;
    pub const CORE_INVALID_ARGUMENT: u32 = 0x8001_000a;
    pub const CORE_ACCESS_DENIED: u32 = 0x8001_000c;
    pub const RV_INVALID_USERNAME: u32 = 0x8003_0064;
    pub const RV_INVALID_PASSWORD: u32 = 0x8003_006b;
    pub const RV_SESSION_VOID: u32 = 0x8003_006e;
    pub const RV_CONNECTION_FAILURE: u32 = 0x8003_0066;
    pub const RV_ACCOUNT_EXPIRED: u32 = 0x8003_0069;
}

#[derive(Debug, Clone, PartialEq)]
pub struct Request {
    pub protocol: u16,
    pub call_id: u32,
    pub method: u32,
    pub body: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Message {
    Request(Request),
    /// A client's answer to a request the server sent (notifications have none; kept for completeness).
    Response { protocol: u16, call_id: u32, result: std::result::Result<(u32, Vec<u8>), u32> },
}

fn write_protocol(w: &mut Writer, protocol: u16, request: bool) {
    let flag = if request { 0x80 } else { 0 };
    if protocol < 0x7f {
        w.u8(protocol as u8 | flag);
    } else {
        w.u8(0x7f | flag);
        w.u16(protocol);
    }
}
fn finish(mut w: Writer) -> Vec<u8> {
    let len = (w.buf.len() - 4) as u32;
    w.buf[..4].copy_from_slice(&len.to_le_bytes());
    w.buf
}

pub fn encode_request(protocol: u16, call_id: u32, method: u32, body: &[u8]) -> Vec<u8> {
    let mut w = Writer::new();
    w.u32(0);
    write_protocol(&mut w, protocol, true);
    w.u32(call_id).u32(method).bytes(body);
    finish(w)
}
pub fn encode_success(protocol: u16, call_id: u32, method: u32, data: &[u8]) -> Vec<u8> {
    let mut w = Writer::new();
    w.u32(0);
    write_protocol(&mut w, protocol, false);
    w.u8(1).u32(call_id).u32(method | 0x8000).bytes(data);
    finish(w)
}
pub fn encode_error(protocol: u16, call_id: u32, code: u32) -> Vec<u8> {
    let mut w = Writer::new();
    w.u32(0);
    write_protocol(&mut w, protocol, false);
    w.u8(0).u32(code | 0x8000_0000).u32(call_id);
    finish(w)
}

/// Decodes one message; returns it and the bytes it used (a payload may hold several).
pub fn decode(data: &[u8]) -> Result<(Message, usize)> {
    let mut r = Reader::new(data);
    let size = r.u32()? as usize;
    let body = r.take(size)?;
    let used = 4 + size;
    let mut r = Reader::new(body);
    let p = r.u8()?;
    let request = p & 0x80 != 0;
    let mut protocol = (p & 0x7f) as u16;
    if protocol == 0x7f {
        protocol = r.u16()?;
    }
    if request {
        let call_id = r.u32()?;
        let method = r.u32()?;
        return Ok((Message::Request(Request { protocol, call_id, method, body: r.rest().to_vec() }), used));
    }
    let ok = r.u8()?;
    let msg = if ok == 1 {
        let call_id = r.u32()?;
        let method = r.u32()? & !0x8000;
        Message::Response { protocol, call_id, result: Ok((method, r.rest().to_vec())) }
    } else if ok == 0 {
        let code = r.u32()?;
        let call_id = r.u32()?;
        Message::Response { protocol, call_id, result: Err(code) }
    } else {
        return Err(DecodeError("bad RMC response flag"));
    };
    Ok((msg, used))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn request_roundtrip() {
        let m = encode_request(10, 7, 2, &[1, 2, 3]);
        assert_eq!(&m[..], &[12, 0, 0, 0, 0x8a, 7, 0, 0, 0, 2, 0, 0, 0, 1, 2, 3]);
        let (d, used) = decode(&m).unwrap();
        assert_eq!(used, m.len());
        assert_eq!(d, Message::Request(Request { protocol: 10, call_id: 7, method: 2, body: vec![1, 2, 3] }));
    }
    #[test]
    fn extended_protocol() {
        let m = encode_request(0x1234, 1, 5, &[]);
        assert_eq!(m[4], 0xff);
        let (d, _) = decode(&m).unwrap();
        assert!(matches!(d, Message::Request(Request { protocol: 0x1234, method: 5, .. })));
    }
    #[test]
    fn responses() {
        let ok = encode_success(11, 9, 4, &[0xaa]);
        assert_eq!(&ok[..], &[11, 0, 0, 0, 11, 1, 9, 0, 0, 0, 4, 0x80, 0, 0, 0xaa]);
        assert_eq!(decode(&ok).unwrap().0, Message::Response { protocol: 11, call_id: 9, result: Ok((4, vec![0xaa])) });
        let e = encode_error(109, 3, err::CORE_NOT_IMPLEMENTED);
        assert_eq!(decode(&e).unwrap().0, Message::Response { protocol: 109, call_id: 3, result: Err(0x8001_0002) });
        // two messages in one payload
        let mut both = ok.clone();
        both.extend_from_slice(&e);
        let (_, used) = decode(&both).unwrap();
        assert_eq!(used, ok.len());
        assert!(decode(&both[used..]).is_ok());
        assert!(decode(&ok[..ok.len() - 1]).is_err());
    }
}
