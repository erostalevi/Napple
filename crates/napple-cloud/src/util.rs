use base64::Engine;
use rand_core::{OsRng, TryRngCore};
use sha2::{Digest, Sha256};

/// Binary fields on the wire (public keys, wrapped keys, salts) are base64url without padding; decoding also accepts standard base64 and padding.
pub fn b64(data: &[u8]) -> String {
    base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(data)
}

pub fn unb64(s: &str) -> Option<Vec<u8>> {
    let t = s.trim().trim_end_matches('=');
    base64::engine::general_purpose::URL_SAFE_NO_PAD
        .decode(t)
        .or_else(|_| base64::engine::general_purpose::STANDARD_NO_PAD.decode(t))
        .ok()
}

pub fn sha256(data: &[u8]) -> [u8; 32] {
    Sha256::digest(data).into()
}

pub fn hex(data: &[u8]) -> String {
    data.iter().map(|b| format!("{b:02x}")).collect()
}

pub fn unhex(s: &str) -> Option<Vec<u8>> {
    if s.len() % 2 != 0 {
        return None;
    }
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).ok()).collect()
}

pub fn sha256_hex(data: &[u8]) -> String {
    hex(&sha256(data))
}

pub fn random<const N: usize>() -> [u8; N] {
    let mut b = [0u8; N];
    OsRng.try_fill_bytes(&mut b).expect("the OS random generator failed");
    b
}

pub fn now() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

/// Title ids are 16 hex digits, upper case (the runtime's g_cfg.title_id form).
pub fn normalize_title(t: &str) -> Option<String> {
    let t = t.trim().trim_start_matches("0x");
    (t.len() == 16 && t.bytes().all(|c| c.is_ascii_hexdigit())).then(|| t.to_ascii_uppercase())
}

/// Profile ids are used in file names and HKDF info: keep them to [A-Za-z0-9-_].
pub fn valid_id(s: &str) -> bool {
    !s.is_empty() && s.len() <= 64 && s.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'-' || c == b'_')
}
