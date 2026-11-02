//! The server's front door: one TCP port speaking HTTP/1.1 for the account API and WebSocket (RFC 6455) for the game server.
//!
//!   POST /napple/v1/account  uid=<32 hex>                          -> pid=<u64>\nsecret=<hex>\n
//!   POST /napple/v1/token    pid=<u64>&secret=<hex>&title=<16 hex>  -> token=<token>\nexpires=<unix seconds>\n
//!   GET  /napple/v1/info                                            -> service=napple-online\nversion=<v>\n
//!   GET  /healthz                                                   -> ok
//!   GET  <any path> with Upgrade: websocket                         -> the NEX game server (PRUDP Lite inside binary messages)
//!
//! TLS is left to a reverse proxy in front of a public server (any https:// proxy that forwards WebSocket upgrades); a server on a LAN or on
//! the same Mac is used over plain http://.

use crate::accounts::{now_secs, AccountError, Accounts};
use crate::crypto::{base64, sha1};
use std::collections::HashMap;
use std::io::{self, Read, Write};

pub struct HttpRequest {
    pub method: String,
    pub path: String,
    pub headers: HashMap<String, String>,
    pub body: Vec<u8>,
}

impl HttpRequest {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers.get(&name.to_ascii_lowercase()).map(|s| s.as_str())
    }
    pub fn is_websocket(&self) -> bool {
        self.method == "GET"
            && self.header("upgrade").is_some_and(|v| v.eq_ignore_ascii_case("websocket"))
            && self.header("sec-websocket-key").is_some()
    }
}

/// Reads one request (head + Content-Length body). `pre` = bytes already read past the request are returned with it (pipelined data).
pub fn read_request(s: &mut impl Read) -> io::Result<(HttpRequest, Vec<u8>)> {
    let mut buf = Vec::new();
    let mut tmp = [0u8; 4096];
    let head_end = loop {
        if let Some(i) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
            break i;
        }
        if buf.len() > 64 * 1024 {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "request head too large"));
        }
        let n = s.read(&mut tmp)?;
        if n == 0 {
            return Err(io::ErrorKind::UnexpectedEof.into());
        }
        buf.extend_from_slice(&tmp[..n]);
    };
    let head = String::from_utf8_lossy(&buf[..head_end]).into_owned();
    let mut lines = head.split("\r\n");
    let first = lines.next().unwrap_or("");
    let mut parts = first.split_whitespace();
    let method = parts.next().unwrap_or("").to_string();
    let path = parts.next().unwrap_or("/").to_string();
    let mut headers = HashMap::new();
    for l in lines {
        if let Some((k, v)) = l.split_once(':') {
            headers.insert(k.trim().to_ascii_lowercase(), v.trim().to_string());
        }
    }
    let mut rest = buf[head_end + 4..].to_vec();
    let len: usize = headers.get("content-length").and_then(|v| v.parse().ok()).unwrap_or(0);
    if len > 1024 * 1024 {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "body too large"));
    }
    while rest.len() < len {
        let n = s.read(&mut tmp)?;
        if n == 0 {
            return Err(io::ErrorKind::UnexpectedEof.into());
        }
        rest.extend_from_slice(&tmp[..n]);
    }
    let extra = rest.split_off(len);
    Ok((HttpRequest { method, path, headers, body: rest }, extra))
}

pub fn write_response(s: &mut impl Write, status: u16, reason: &str, body: &str) -> io::Result<()> {
    write!(s, "HTTP/1.1 {status} {reason}\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}", body.len())?;
    s.flush()
}

/// application/x-www-form-urlencoded (the runtime only sends hex and digits; %XX and + are decoded anyway).
pub fn parse_form(body: &[u8]) -> HashMap<String, String> {
    let dec = |s: &str| -> String {
        let b = s.as_bytes();
        let mut out = Vec::new();
        let mut i = 0;
        while i < b.len() {
            match b[i] {
                b'+' => out.push(b' '),
                b'%' if i + 2 < b.len() => {
                    if let Ok(v) = u8::from_str_radix(&s[i + 1..i + 3], 16) {
                        out.push(v);
                        i += 2;
                    } else {
                        out.push(b'%');
                    }
                }
                c => out.push(c),
            }
            i += 1;
        }
        String::from_utf8_lossy(&out).into_owned()
    };
    String::from_utf8_lossy(body)
        .split('&')
        .filter_map(|kv| kv.split_once('=').map(|(k, v)| (dec(k), dec(v))))
        .collect()
}

/// The account API. Returns (status, reason, body).
pub fn handle_api(acc: &Accounts, req: &HttpRequest) -> (u16, &'static str, String) {
    let form = parse_form(&req.body);
    match (req.method.as_str(), req.path.split('?').next().unwrap_or("")) {
        ("GET", "/healthz") => (200, "OK", "ok\n".into()),
        ("GET", "/napple/v1/info") => (200, "OK", format!(
            "service=napple-online\nversion={}\npublic_host={}\n",
            env!("CARGO_PKG_VERSION"),
            std::env::var("NAPPLE_ONLINE_PUBLIC_HOST").unwrap_or_default()
        )),
        ("POST", "/napple/v1/account") => match acc.create(form.get("uid").map(|s| s.as_str()).unwrap_or("")) {
            Ok((a, secret)) => (200, "OK", format!("pid={}\nsecret={secret}\n", a.pid)),
            Err(AccountError::BadRequest) => (400, "Bad Request", "error=uid must be 32 hex digits\n".into()),
            Err(e) => (500, "Internal Server Error", format!("error={e:?}\n")),
        },
        ("POST", "/napple/v1/token") => {
            let pid = form.get("pid").and_then(|v| v.parse::<u64>().ok()).unwrap_or(0);
            let title = form.get("title").cloned().unwrap_or_default();
            if title.len() != 16 || !title.bytes().all(|c| c.is_ascii_hexdigit()) {
                return (400, "Bad Request", "error=title must be 16 hex digits\n".into());
            }
            match acc.authenticate(pid, form.get("secret").map(|s| s.as_str()).unwrap_or("")) {
                Ok(_) => {
                    let now = now_secs();
                    let t = acc.issue_token(pid, &title, now);
                    (200, "OK", format!("token={t}\nexpires={}\n", now + crate::accounts::TOKEN_LIFETIME_SECS))
                }
                Err(_) => (401, "Unauthorized", "error=unknown account or wrong secret\n".into()),
            }
        }
        _ => (404, "Not Found", "error=not found\n".into()),
    }
}

/// The WebSocket handshake answer for `req` (echoing the first subprotocol the client offers).
pub fn websocket_accept(req: &HttpRequest) -> String {
    let key = req.header("sec-websocket-key").unwrap_or("");
    let accept = base64(&sha1(format!("{key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11").as_bytes()), false);
    let mut r = format!("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n");
    if let Some(p) = req.header("sec-websocket-protocol") {
        let first = p.split(',').next().unwrap_or("").trim();
        if !first.is_empty() {
            r.push_str(&format!("Sec-WebSocket-Protocol: {first}\r\n"));
        }
    }
    r.push_str("\r\n");
    r
}

pub const WS_TEXT: u8 = 1;
pub const WS_BINARY: u8 = 2;
pub const WS_CLOSE: u8 = 8;
pub const WS_PING: u8 = 9;
pub const WS_PONG: u8 = 10;

/// A server frame (never masked, always FIN).
pub fn ws_frame(opcode: u8, payload: &[u8]) -> Vec<u8> {
    let mut f = vec![0x80 | opcode];
    match payload.len() {
        n if n < 126 => f.push(n as u8),
        n if n <= 0xffff => {
            f.push(126);
            f.extend_from_slice(&(n as u16).to_be_bytes());
        }
        n => {
            f.push(127);
            f.extend_from_slice(&(n as u64).to_be_bytes());
        }
    }
    f.extend_from_slice(payload);
    f
}

/// Incremental frame parser: feed bytes, take complete messages (fragmented messages are joined; control frames come out as they arrive).
#[derive(Default)]
pub struct WsReader {
    buf: Vec<u8>,
    partial: Vec<u8>,
    partial_op: u8,
}
impl WsReader {
    pub fn feed(&mut self, data: &[u8]) {
        self.buf.extend_from_slice(data);
    }
    /// The next complete message (opcode, payload), or None until more bytes arrive. Err on a protocol violation.
    pub fn next(&mut self) -> Result<Option<(u8, Vec<u8>)>, &'static str> {
        loop {
            let b = &self.buf;
            if b.len() < 2 {
                return Ok(None);
            }
            let fin = b[0] & 0x80 != 0;
            let op = b[0] & 15;
            let masked = b[1] & 0x80 != 0;
            let (len, mut at) = match b[1] & 127 {
                126 if b.len() >= 4 => (u16::from_be_bytes([b[2], b[3]]) as usize, 4),
                127 if b.len() >= 10 => (u64::from_be_bytes(b[2..10].try_into().unwrap()) as usize, 10),
                126 | 127 => return Ok(None),
                n => (n as usize, 2),
            };
            if len > 16 * 1024 * 1024 {
                return Err("frame too large");
            }
            let mask = if masked {
                if b.len() < at + 4 {
                    return Ok(None);
                }
                at += 4;
                Some([b[at - 4], b[at - 3], b[at - 2], b[at - 1]])
            } else {
                None
            };
            if b.len() < at + len {
                return Ok(None);
            }
            let mut payload = b[at..at + len].to_vec();
            if let Some(m) = mask {
                for (i, x) in payload.iter_mut().enumerate() {
                    *x ^= m[i % 4];
                }
            }
            self.buf.drain(..at + len);
            if op >= 8 {
                return Ok(Some((op, payload)));
            }
            if op != 0 {
                self.partial_op = op;
                self.partial.clear();
            }
            self.partial.extend_from_slice(&payload);
            if fin {
                return Ok(Some((self.partial_op, std::mem::take(&mut self.partial))));
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn request_parsing_and_api() {
        let raw = b"POST /napple/v1/account HTTP/1.1\r\nHost: x\r\nContent-Length: 36\r\n\r\nuid=4e6170706c6555736572000000000001EXTRA";
        let (req, extra) = read_request(&mut &raw[..]).unwrap();
        assert_eq!((req.method.as_str(), req.path.as_str()), ("POST", "/napple/v1/account"));
        assert_eq!(extra, b"EXTRA");
        let acc = Accounts::open(None).unwrap();
        let (st, _, body) = handle_api(&acc, &req);
        assert_eq!(st, 200);
        let mut kv = HashMap::new();
        for l in body.lines() {
            let (k, v) = l.split_once('=').unwrap();
            kv.insert(k.to_string(), v.to_string());
        }
        let tok_req = HttpRequest {
            method: "POST".into(),
            path: "/napple/v1/token".into(),
            headers: HashMap::new(),
            body: format!("pid={}&secret={}&title=0100152000022000", kv["pid"], kv["secret"]).into_bytes(),
        };
        let (st, _, body) = handle_api(&acc, &tok_req);
        assert_eq!(st, 200);
        let token = body.lines().next().unwrap().strip_prefix("token=").unwrap();
        assert_eq!(acc.verify_token(token, now_secs()).unwrap().pid, kv["pid"].parse::<u64>().unwrap());
        let bad = HttpRequest { body: format!("pid={}&secret=00&title=0100152000022000", kv["pid"]).into_bytes(), ..tok_req };
        assert_eq!(handle_api(&acc, &bad).0, 401);
        let health = HttpRequest { method: "GET".into(), path: "/healthz".into(), headers: HashMap::new(), body: vec![] };
        assert_eq!(handle_api(&acc, &health).0, 200);
        assert_eq!(parse_form(b"a=1%2B2&b=x+y")["a"], "1+2");
        assert_eq!(parse_form(b"a=1%2B2&b=x+y")["b"], "x y");
    }

    #[test]
    fn websocket_handshake_and_frames() {
        let raw = b"GET / HTTP/1.1\r\nHost: g-lp1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Protocol: NEX, other\r\n\r\n";
        let (req, _) = read_request(&mut &raw[..]).unwrap();
        assert!(req.is_websocket());
        let a = websocket_accept(&req);
        assert!(a.contains("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"));
        assert!(a.contains("Sec-WebSocket-Protocol: NEX\r\n"));
        // a masked client frame split across feeds, then a fragmented message and a ping in between
        let mut r = WsReader::default();
        let masked = [0x82, 0x83, 1, 2, 3, 4, 0xa ^ 1, 0xb ^ 2, 0xc ^ 3];
        r.feed(&masked[..5]);
        assert_eq!(r.next().unwrap(), None);
        r.feed(&masked[5..]);
        assert_eq!(r.next().unwrap(), Some((WS_BINARY, vec![0xa, 0xb, 0xc])));
        r.feed(&[0x02, 1, 0xaa]);
        r.feed(&ws_frame(WS_PING, b"p"));
        r.feed(&[0x80, 1, 0xbb]);
        assert_eq!(r.next().unwrap(), Some((WS_PING, b"p".to_vec())));
        assert_eq!(r.next().unwrap(), Some((WS_BINARY, vec![0xaa, 0xbb])));
        let big = vec![7u8; 70000];
        r.feed(&ws_frame(WS_BINARY, &big));
        assert_eq!(r.next().unwrap().unwrap().1.len(), 70000);
        assert_eq!(&ws_frame(WS_BINARY, &[0; 200])[..4], &[0x82, 126, 0, 200]);
    }
}
