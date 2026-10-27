//! Small shared helpers: hex, build ids, hashing, safe paths, errors.
use sha2::{Digest, Sha256};
use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModError(pub String);

impl fmt::Display for ModError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for ModError {}

pub type Result<T> = std::result::Result<T, ModError>;

pub fn err<T>(m: impl Into<String>) -> Result<T> {
    Err(ModError(m.into()))
}

impl From<std::io::Error> for ModError {
    fn from(e: std::io::Error) -> Self {
        ModError(format!("io: {e}"))
    }
}

impl From<serde_json::Error> for ModError {
    fn from(e: serde_json::Error) -> Self {
        ModError(format!("json: {e}"))
    }
}

impl From<zip::result::ZipError> for ModError {
    fn from(e: zip::result::ZipError) -> Self {
        ModError(format!("zip: {e}"))
    }
}

pub fn hex_encode(b: &[u8]) -> String {
    let mut s = String::with_capacity(b.len() * 2);
    for x in b {
        s.push_str(&format!("{x:02x}"));
    }
    s
}

pub fn hex_decode(s: &str) -> Result<Vec<u8>> {
    let s = s.trim();
    if !s.len().is_multiple_of(2) || !s.is_ascii() {
        return err(format!("bad hex string {s:?}"));
    }
    (0..s.len() / 2)
        .map(|i| {
            u8::from_str_radix(&s[2 * i..2 * i + 2], 16)
                .map_err(|_| ModError(format!("bad hex string {s:?}")))
        })
        .collect()
}

pub fn sha256_hex(b: &[u8]) -> String {
    hex_encode(&Sha256::digest(b))
}

/// A module build id: 1..=64 hex digits, padded with zeros on the right to 32 bytes
/// (Atmosphere IPS file names and pchtxt `@nsobid-` often drop trailing zeros).
pub fn norm_build_id(s: &str) -> Result<[u8; 32]> {
    let t = s.trim();
    if t.is_empty() || t.len() > 64 || !t.bytes().all(|c| c.is_ascii_hexdigit()) {
        return err(format!("bad build id {s:?} (1..=64 hex digits)"));
    }
    let mut padded = t.to_ascii_lowercase();
    while padded.len() < 64 {
        padded.push('0');
    }
    let v = hex_decode(&padded)?;
    let mut o = [0u8; 32];
    o.copy_from_slice(&v);
    Ok(o)
}

pub fn build_id_hex(b: &[u8; 32]) -> String {
    hex_encode(b).to_ascii_uppercase()
}

/// Accepts a path inside a mod archive or a romfs path component list.
/// Rejects absolute paths, `..`, `.`, empty components, backslashes, NUL/control characters and drive letters.
pub fn safe_rel_path(p: &str) -> Result<()> {
    if p.is_empty() {
        return err("empty path");
    }
    if p.len() > 1024 {
        return err("path too long");
    }
    if p.starts_with('/') || p.contains('\\') {
        return err(format!("absolute or backslash path {p:?}"));
    }
    for c in p.split('/') {
        if c.is_empty() || c == "." || c == ".." {
            return err(format!("path traversal or empty component in {p:?}"));
        }
        if c.chars().any(|ch| (ch as u32) < 0x20 || ch == '\u{7f}') {
            return err(format!("control character in path {p:?}"));
        }
        let b = c.as_bytes();
        if b.len() >= 2 && b[1] == b':' && b[0].is_ascii_alphabetic() {
            return err(format!("drive letter in path {p:?}"));
        }
    }
    Ok(())
}

#[cfg(test)]
pub fn xorshift(state: &mut u64) -> u64 {
    let mut x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    x
}
