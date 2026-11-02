//! A minimal blocking HTTP/1.1 client for the two services napple-online calls (napple-account, napple-social), which listen on localhost
//! behind the reverse proxy. Plain `http://` only: a TLS URL is refused (point the env at the local listener). `Upstream` is a trait so
//! tests run against an in-memory mock.

use std::io::{Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::time::Duration;

pub trait Upstream: Send + Sync {
    /// Returns (status, body).
    fn request(&self, method: &str, url: &str, bearer: Option<&str>, headers: &[(&str, &str)], body: Option<&str>) -> Result<(u16, Vec<u8>), String>;
}

pub struct HttpUpstream;

/// Splits `http://host:port/path` into (host:port, path).
pub fn split_url(url: &str) -> Result<(String, String), String> {
    let rest = url.strip_prefix("http://").ok_or_else(|| format!("only http:// upstreams are supported, got {url}"))?;
    let (host, path) = match rest.find('/') {
        Some(i) => (&rest[..i], &rest[i..]),
        None => (rest, "/"),
    };
    let host = if host.contains(':') { host.to_string() } else { format!("{host}:80") };
    Ok((host, path.to_string()))
}

impl Upstream for HttpUpstream {
    fn request(&self, method: &str, url: &str, bearer: Option<&str>, headers: &[(&str, &str)], body: Option<&str>) -> Result<(u16, Vec<u8>), String> {
        let (host, path) = split_url(url)?;
        let addr = host.to_socket_addrs().map_err(|e| e.to_string())?.next().ok_or("no address")?;
        let mut s = TcpStream::connect_timeout(&addr, Duration::from_secs(3)).map_err(|e| e.to_string())?;
        s.set_read_timeout(Some(Duration::from_secs(5))).ok();
        s.set_write_timeout(Some(Duration::from_secs(5))).ok();
        let mut req = format!("{method} {path} HTTP/1.1\r\nHost: {host}\r\nUser-Agent: napple-online\r\nConnection: close\r\nAccept: application/json\r\n");
        if let Some(b) = bearer {
            req.push_str(&format!("Authorization: Bearer {b}\r\n"));
        }
        for (k, v) in headers {
            req.push_str(&format!("{k}: {v}\r\n"));
        }
        let body = body.unwrap_or("");
        if method != "GET" {
            req.push_str(&format!("Content-Type: application/json\r\nContent-Length: {}\r\n", body.len()));
        }
        req.push_str("\r\n");
        req.push_str(body);
        s.write_all(req.as_bytes()).map_err(|e| e.to_string())?;
        let mut raw = Vec::new();
        s.take(4 << 20).read_to_end(&mut raw).map_err(|e| e.to_string())?;
        let end = raw.windows(4).position(|w| w == b"\r\n\r\n").ok_or("truncated response")?;
        let head = String::from_utf8_lossy(&raw[..end]).to_string();
        let status: u16 = head.split_whitespace().nth(1).and_then(|c| c.parse().ok()).ok_or("bad status line")?;
        let mut body = raw[end + 4..].to_vec();
        if head.to_ascii_lowercase().contains("transfer-encoding: chunked") {
            body = unchunk(&body)?;
        }
        Ok((status, body))
    }
}

fn unchunk(mut b: &[u8]) -> Result<Vec<u8>, String> {
    let mut out = Vec::new();
    loop {
        let nl = b.windows(2).position(|w| w == b"\r\n").ok_or("bad chunk")?;
        let n = usize::from_str_radix(String::from_utf8_lossy(&b[..nl]).split(';').next().unwrap_or("").trim(), 16).map_err(|_| "bad chunk size")?;
        b = &b[nl + 2..];
        if n == 0 {
            return Ok(out);
        }
        if b.len() < n + 2 {
            return Err("short chunk".into());
        }
        out.extend_from_slice(&b[..n]);
        b = &b[n + 2..];
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpListener;

    #[test]
    fn split() {
        assert_eq!(split_url("http://127.0.0.1:9/a/b").unwrap(), ("127.0.0.1:9".into(), "/a/b".into()));
        assert_eq!(split_url("http://x").unwrap(), ("x:80".into(), "/".into()));
        assert!(split_url("https://x").is_err());
    }

    #[test]
    fn get_and_chunked_post() {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = l.local_addr().unwrap().port();
        let t = std::thread::spawn(move || {
            for i in 0..2 {
                let (mut c, _) = l.accept().unwrap();
                let mut buf = [0u8; 4096];
                let n = c.read(&mut buf).unwrap();
                let req = String::from_utf8_lossy(&buf[..n]).to_string();
                assert!(req.contains("Authorization: Bearer tok"));
                if i == 0 {
                    assert!(req.starts_with("GET /x "));
                    c.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi").unwrap();
                } else {
                    assert!(req.starts_with("POST /y ") && req.ends_with("{\"a\":1}"));
                    c.write_all(b"HTTP/1.1 201 Created\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n").unwrap();
                }
            }
        });
        let u = HttpUpstream;
        assert_eq!(u.request("GET", &format!("http://127.0.0.1:{port}/x"), Some("tok"), &[], None).unwrap(), (200, b"hi".to_vec()));
        assert_eq!(u.request("POST", &format!("http://127.0.0.1:{port}/y"), Some("tok"), &[], Some("{\"a\":1}")).unwrap(), (201, b"abcde".to_vec()));
        t.join().unwrap();
    }
}
