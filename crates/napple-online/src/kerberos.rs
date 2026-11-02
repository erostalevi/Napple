//! NEX "Kerberos" tickets: how the Authentication server hands a client the session key for the Secure server.
//!
//! A Kerberos blob is RC4(key, data) followed by HMAC-MD5(key, ciphertext). The client's ticket is sealed with a per-login source key the server
//! chooses and returns as a hex string (Switch titles skip their own key derivation when it is present), so no password or console secret is
//! involved:
//!   ticket = blob(source key, session key (32) | target pid (u64) | Buffer internal ticket)
//! The internal ticket is only ever read back by this server: blob(secure key, issue time (u64 unix s) | source pid (u64) | session key (32)).
//! On CONNECT to the secure server the client sends Buffer internal ticket | Buffer blob(session key, pid (u64) | connection id (u32) | check (u32))
//! and gets back Buffer(check + 1).

use crate::crypto::{ct_eq, hmac_md5, md5, Rc4};
use crate::nex::{DecodeError, Reader, Result, Writer};

pub const SESSION_KEY_LEN: usize = 32;
/// How long an internal ticket is accepted after it was issued.
pub const TICKET_LIFETIME_SECS: u64 = 120;

pub fn encrypt(key: &[u8], plain: &[u8]) -> Vec<u8> {
    let mut data = plain.to_vec();
    Rc4::new(key).apply(&mut data);
    let mac = hmac_md5(key, &data);
    data.extend_from_slice(&mac);
    data
}
pub fn decrypt(key: &[u8], blob: &[u8]) -> Result<Vec<u8>> {
    if blob.len() < 16 {
        return Err(DecodeError("kerberos blob too short"));
    }
    let (data, mac) = blob.split_at(blob.len() - 16);
    if !ct_eq(&hmac_md5(key, data), mac) {
        return Err(DecodeError("kerberos checksum mismatch"));
    }
    let mut out = data.to_vec();
    Rc4::new(key).apply(&mut out);
    Ok(out)
}

/// The key internal tickets are sealed with, derived from the server key.
pub fn secure_key(server_key: &[u8]) -> [u8; 16] {
    let mut v = server_key.to_vec();
    v.extend_from_slice(b"napple-online secure server tickets");
    md5(&v)
}

pub fn internal_ticket(secure_key: &[u8], issued: u64, source_pid: u64, session_key: &[u8]) -> Vec<u8> {
    let mut w = Writer::new();
    w.u64(issued).pid(source_pid).bytes(session_key);
    encrypt(secure_key, &w.buf)
}

/// The ticket buffer of a ValidateAndRequestTicket* / RequestTicket answer.
pub fn client_ticket(source_key: &[u8], session_key: &[u8], target_pid: u64, internal: &[u8]) -> Vec<u8> {
    let mut w = Writer::new();
    w.bytes(session_key).pid(target_pid).buffer(internal);
    encrypt(source_key, &w.buf)
}

/// What the client side decodes from its ticket (tests, the tool's self-check).
pub fn open_client_ticket(source_key: &[u8], ticket: &[u8]) -> Result<(Vec<u8>, u64, Vec<u8>)> {
    let plain = decrypt(source_key, ticket)?;
    let mut r = Reader::new(&plain);
    let key = r.take(SESSION_KEY_LEN)?.to_vec();
    let pid = r.pid()?;
    let internal = r.buffer()?;
    Ok((key, pid, internal))
}

#[derive(Debug, Clone, PartialEq)]
pub struct SecureConnect {
    pub pid: u64,
    pub connection_id: u32,
    pub check: u32,
    pub session_key: Vec<u8>,
}

/// Checks a secure CONNECT payload at time `now`.
pub fn open_secure_connect(secure_key: &[u8], payload: &[u8], now: u64) -> Result<SecureConnect> {
    let mut r = Reader::new(payload);
    let ticket = r.buffer()?;
    let request = r.buffer()?;
    let t = decrypt(secure_key, &ticket)?;
    let mut tr = Reader::new(&t);
    let issued = tr.u64()?;
    let ticket_pid = tr.pid()?;
    let session_key = tr.take(SESSION_KEY_LEN)?.to_vec();
    if now > issued + TICKET_LIFETIME_SECS || issued > now + 60 {
        return Err(DecodeError("ticket expired"));
    }
    let req = decrypt(&session_key, &request)?;
    let mut rr = Reader::new(&req);
    let pid = rr.pid()?;
    let connection_id = rr.u32()?;
    let check = rr.u32()?;
    if pid != ticket_pid {
        return Err(DecodeError("ticket of another user"));
    }
    Ok(SecureConnect { pid, connection_id, check, session_key })
}

pub fn secure_connect_response(check: u32) -> Vec<u8> {
    let mut w = Writer::new();
    w.u32(4).u32(check.wrapping_add(1));
    w.buf
}

/// The client half of a secure CONNECT (tests; mirrors what a title sends).
pub fn client_secure_connect(internal: &[u8], session_key: &[u8], pid: u64, cid: u32, check: u32) -> Vec<u8> {
    let mut req = Writer::new();
    req.pid(pid).u32(cid).u32(check);
    let mut w = Writer::new();
    w.buffer(internal).buffer(&encrypt(session_key, &req.buf));
    w.buf
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::crypto::random_bytes;

    #[test]
    fn blob_roundtrip_and_tamper() {
        let key = b"0123456789abcdef";
        let b = encrypt(key, b"hello");
        assert_eq!(b.len(), 5 + 16);
        assert_eq!(decrypt(key, &b).unwrap(), b"hello");
        let mut bad = b.clone();
        bad[0] ^= 1;
        assert!(decrypt(key, &bad).is_err());
        assert!(decrypt(b"another key.....", &b).is_err());
    }

    #[test]
    fn ticket_flow() {
        let server_key = random_bytes(32);
        let sk = secure_key(&server_key);
        let source_key = random_bytes(16);
        let session = random_bytes(SESSION_KEY_LEN);
        let pid = 0x4e41_0000_0000_1000;
        let internal = internal_ticket(&sk, 1000, pid, &session);
        let ticket = client_ticket(&source_key, &session, 2, &internal);
        // the client opens its ticket with the source key
        let (k, target, inner) = open_client_ticket(&source_key, &ticket).unwrap();
        assert_eq!((k.as_slice(), target, inner.as_slice()), (session.as_slice(), 2, internal.as_slice()));
        // and presents the internal ticket to the secure server
        let payload = client_secure_connect(&inner, &k, pid, 77, 0x1234);
        let c = open_secure_connect(&sk, &payload, 1010).unwrap();
        assert_eq!(c, SecureConnect { pid, connection_id: 77, check: 0x1234, session_key: session.clone() });
        assert_eq!(secure_connect_response(0x1234), vec![4, 0, 0, 0, 0x35, 0x12, 0, 0]);
        // expired, another user's request, a ticket from another server
        assert!(open_secure_connect(&sk, &payload, 1000 + TICKET_LIFETIME_SECS + 1).is_err());
        let wrong = client_secure_connect(&inner, &k, pid + 1, 77, 1);
        assert!(open_secure_connect(&sk, &wrong, 1010).is_err());
        assert!(open_secure_connect(&secure_key(&random_bytes(32)), &payload, 1010).is_err());
    }
}
