//! Small helpers: base64url, random tokens, hashing, constant-time compares, UTC dates, input validation.

use base64::Engine;
use base64::engine::general_purpose::{STANDARD, STANDARD_NO_PAD, URL_SAFE, URL_SAFE_NO_PAD};
use hmac::{Hmac, Mac};
use rand::RngCore;
use sha2::{Digest, Sha256};
use subtle::ConstantTimeEq;

pub type HmacSha256 = Hmac<Sha256>;

/// Every binary field in the API is emitted as base64url without padding.
pub fn b64url(data: &[u8]) -> String {
    URL_SAFE_NO_PAD.encode(data)
}

/// Lenient decoder for client input: base64url or standard alphabet, padded or not.
pub fn b64_decode(s: &str) -> Option<Vec<u8>> {
    let s = s.trim();
    URL_SAFE_NO_PAD
        .decode(s)
        .or_else(|_| URL_SAFE.decode(s))
        .or_else(|_| STANDARD.decode(s))
        .or_else(|_| STANDARD_NO_PAD.decode(s))
        .ok()
}

pub fn b64std(data: &[u8]) -> String {
    STANDARD.encode(data)
}

pub fn random_bytes<const N: usize>() -> [u8; N] {
    let mut b = [0u8; N];
    rand::rngs::OsRng.fill_bytes(&mut b);
    b
}

/// 256-bit random opaque token, base64url.
pub fn random_token() -> String {
    b64url(&random_bytes::<32>())
}

/// Uniform 6-digit code (rejection sampling, no modulo bias).
pub fn random_code() -> String {
    loop {
        let v = u32::from_le_bytes(random_bytes::<4>());
        if v < 4_294_000_000 {
            return format!("{:06}", v % 1_000_000);
        }
    }
}

pub fn sha256(data: &[u8]) -> [u8; 32] {
    Sha256::digest(data).into()
}

pub fn sha256_hex(data: &[u8]) -> String {
    hex::encode(sha256(data))
}

pub fn hmac_sha256(key: &[u8], data: &[u8]) -> [u8; 32] {
    let mut m = <HmacSha256 as Mac>::new_from_slice(key).expect("hmac accepts any key length");
    m.update(data);
    m.finalize().into_bytes().into()
}

pub fn ct_eq(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && bool::from(a.ct_eq(b))
}

/// Hash used for stored secrets (refresh tokens, link tokens): never store the secret itself.
pub fn token_hash(token: &str) -> Vec<u8> {
    sha256(token.as_bytes()).to_vec()
}

pub fn new_id() -> String {
    uuid::Uuid::now_v7().to_string()
}

/// (year, month, day, hour, minute, second) in UTC for a unix timestamp.
pub fn civil(ts: i64) -> (i64, u32, u32, u32, u32, u32) {
    let days = ts.div_euclid(86_400);
    let secs = ts.rem_euclid(86_400);
    // Howard Hinnant's days_from_civil inverse.
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    let y = yoe + era * 400 + i64::from(m <= 2);
    (
        y,
        m,
        d,
        (secs / 3600) as u32,
        (secs % 3600 / 60) as u32,
        (secs % 60) as u32,
    )
}

/// `20130524T000000Z` (SigV4 date-time).
pub fn amz_date(ts: i64) -> String {
    let (y, mo, d, h, mi, s) = civil(ts);
    format!("{y:04}{mo:02}{d:02}T{h:02}{mi:02}{s:02}Z")
}

/// `2026-11-02` (for human-facing text).
pub fn ymd(ts: i64) -> String {
    let (y, mo, d, ..) = civil(ts);
    format!("{y:04}-{mo:02}-{d:02}")
}

/// Lower-cased, trimmed email if it looks like one address (no display names, no lists).
pub fn normalize_email(s: &str) -> Option<String> {
    let e = s.trim().to_ascii_lowercase();
    if e.len() > 254
        || e.chars().any(|c| {
            c.is_whitespace() || c.is_control() || matches!(c, '<' | '>' | ',' | ';' | '"' | '\\')
        })
    {
        return None;
    }
    let (local, domain) = e.split_once('@')?;
    if local.is_empty()
        || local.len() > 64
        || domain.contains('@')
        || !domain.contains('.')
        || domain.starts_with('.')
        || domain.ends_with('.')
    {
        return None;
    }
    Some(e)
}

/// A user-visible name: 1..=max chars after trimming, no control characters.
pub fn clean_name(s: &str, max: usize) -> Option<String> {
    let t = s.trim();
    let n = t.chars().count();
    if n == 0 || n > max || t.chars().any(char::is_control) {
        return None;
    }
    Some(t.to_string())
}

/// Switch title IDs: 16 hex digits, normalised to upper case.
pub fn normalize_title_id(s: &str) -> Option<String> {
    (s.len() == 16 && s.bytes().all(|b| b.is_ascii_hexdigit())).then(|| s.to_ascii_uppercase())
}

pub fn is_uuid(s: &str) -> bool {
    uuid::Uuid::parse_str(s).is_ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn civil_dates() {
        assert_eq!(amz_date(0), "19700101T000000Z");
        assert_eq!(amz_date(1_369_353_600), "20130524T000000Z");
        assert_eq!(ymd(1_793_577_600), "2026-11-02");
        assert_eq!(amz_date(951_782_400), "20000229T000000Z");
    }

    #[test]
    fn emails() {
        assert_eq!(
            normalize_email(" A@Example.COM ").as_deref(),
            Some("a@example.com")
        );
        assert!(normalize_email("a@b").is_none());
        assert!(normalize_email("x <a@b.c>").is_none());
        assert!(normalize_email("a@b.c,d@e.f").is_none());
    }

    #[test]
    fn codes_are_six_digits() {
        for _ in 0..100 {
            let c = random_code();
            assert_eq!(c.len(), 6);
            assert!(c.bytes().all(|b| b.is_ascii_digit()));
        }
    }

    #[test]
    fn titles() {
        assert_eq!(
            normalize_title_id("0100000000010000").as_deref(),
            Some("0100000000010000")
        );
        assert_eq!(
            normalize_title_id("01006bb00c6f0000").as_deref(),
            Some("01006BB00C6F0000")
        );
        assert!(normalize_title_id("0100").is_none());
        assert!(normalize_title_id("../../etc/passwd").is_none());
    }
}
