//! Napple Account identities: verification of the account service's access JWTs (Ed25519, keys from its JWKS), the acting profile, and the
//! game-visible principal id derived from the profile.
//!
//! * Tokens: compact JWS, `alg` EdDSA (or Ed25519), `kid`, claims `sub` (account UUID), `did`, `aud` "napple", `exp`. Keys come from
//!   `{NAPPLE_ACCOUNT_URL}/account/v1/.well-known/jwks.json`, cached for 10 minutes; an unknown `kid` re-fetches at most every 10 s (a
//!   rotation is picked up, garbage kids cannot hammer the account service); a stale known key keeps working while the account service is down.
//! * Profile: `GET /account/v1/profiles` with the caller's own token, cached 60 s per account; the acting profile must belong to the account.
//! * Principal id (what NEX sees): `pid = 0x4e41_8000_0000_0000 | (SHA-256("napple-online/pid/v1:" + lowercase hyphenated profile_id)[0..6] & 0x7fff_ffff_ffff)`
//!   i.e. "NA" in the top bytes, bit 47 set (far from the legacy counter range 0x4e41_0000_0000_1000..), 47 bits of the hash. Same profile =
//!   same pid on every device and every run; nothing in it reveals the account.

use crate::crypto::{base64_decode, sha256};
use crate::upstream::Upstream;
use ed25519_dalek::{Signature, VerifyingKey};
use serde_json::Value;
use std::collections::HashMap;
use std::sync::{Arc, Mutex};

pub const JWKS_TTL: u64 = 600;
pub const JWKS_MIN_REFRESH: u64 = 10;
pub const PROFILE_TTL: u64 = 60;
pub const PROFILE_MISS_REFETCH: u64 = 5;
pub const LEEWAY: u64 = 30;

#[derive(Clone, Debug, PartialEq)]
pub struct Claims {
    pub account_id: String,
    pub device_id: String,
    pub exp: u64,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Profile {
    pub profile_id: String,
    pub nickname: String,
    pub tier: u8,
}

#[derive(Debug, PartialEq)]
pub enum AuthError {
    Malformed,
    BadSignature,
    Expired,
    WrongAudience,
    UnknownKey,
    NoProfile,
    Unavailable,
}

/// The game-visible principal id of a profile (see the module doc).
pub fn principal_id(profile_id: &str) -> u64 {
    let h = sha256(format!("napple-online/pid/v1:{}", profile_id.to_ascii_lowercase()).as_bytes());
    let mut v = 0u64;
    for b in &h[..6] {
        v = v << 8 | *b as u64;
    }
    0x4e41_8000_0000_0000 | (v & 0x7fff_ffff_ffff)
}

pub fn is_principal(pid: u64) -> bool {
    pid >> 48 == 0x4e41 && pid & 0x8000_0000_0000 != 0
}

fn parse_jwks(doc: &[u8]) -> Option<HashMap<String, VerifyingKey>> {
    let v: Value = serde_json::from_slice(doc).ok()?;
    let mut out = HashMap::new();
    for k in v.get("keys")?.as_array()? {
        if k["kty"] != "OKP" || k["crv"] != "Ed25519" || k.get("use").is_some_and(|u| u != "sig") {
            continue;
        }
        let (Some(kid), Some(x)) = (k["kid"].as_str(), k["x"].as_str()) else { continue };
        let Some(raw) = base64_decode(x).and_then(|r| <[u8; 32]>::try_from(r).ok()) else { continue };
        if let Ok(vk) = VerifyingKey::from_bytes(&raw) {
            out.insert(kid.to_string(), vk);
        }
    }
    Some(out)
}

/// Splits a token into (kid, signing input, signature, claims) after checking the header.
fn parse_token(token: &str) -> Result<(String, &str, Vec<u8>, Value), AuthError> {
    let mut it = token.trim_end_matches('\0').split('.');
    let (Some(h), Some(p), Some(s), None) = (it.next(), it.next(), it.next(), it.next()) else { return Err(AuthError::Malformed) };
    let token = token.trim_end_matches('\0');
    let header: Value = serde_json::from_slice(&base64_decode(h).ok_or(AuthError::Malformed)?).map_err(|_| AuthError::Malformed)?;
    if !matches!(header["alg"].as_str(), Some("EdDSA" | "Ed25519")) {
        return Err(AuthError::Malformed);
    }
    let kid = header["kid"].as_str().filter(|k| !k.is_empty()).ok_or(AuthError::Malformed)?.to_string();
    let claims: Value = serde_json::from_slice(&base64_decode(p).ok_or(AuthError::Malformed)?).map_err(|_| AuthError::Malformed)?;
    Ok((kid, &token[..h.len() + 1 + p.len()], base64_decode(s).ok_or(AuthError::Malformed)?, claims))
}

/// True when `token` looks like an account JWT (EdDSA header), as opposed to a legacy napple-online token.
pub fn looks_like_account_jwt(token: &str) -> bool {
    parse_token(token).is_ok()
}

fn check_claims(c: &Value, audience: &str, now: u64) -> Result<Claims, AuthError> {
    let exp = c["exp"].as_u64().ok_or(AuthError::Malformed)?;
    if now >= exp + LEEWAY {
        return Err(AuthError::Expired);
    }
    if c["nbf"].as_u64().is_some_and(|n| now + LEEWAY < n) {
        return Err(AuthError::Malformed);
    }
    let aud_ok = match &c["aud"] {
        Value::String(a) => a == audience,
        Value::Array(v) => v.iter().any(|a| a == audience),
        _ => false,
    };
    if !aud_ok {
        return Err(AuthError::WrongAudience);
    }
    let sub = c["sub"].as_str().filter(|s| is_uuid(s)).ok_or(AuthError::Malformed)?;
    let did = c["did"].as_str().filter(|d| !d.is_empty()).ok_or(AuthError::Malformed)?;
    Ok(Claims { account_id: sub.to_ascii_lowercase(), device_id: did.to_string(), exp })
}

pub fn is_uuid(s: &str) -> bool {
    s.len() == 36 && s.bytes().enumerate().all(|(i, c)| if matches!(i, 8 | 13 | 18 | 23) { c == b'-' } else { c.is_ascii_hexdigit() })
}

#[derive(Default)]
struct KeyCache {
    keys: HashMap<String, VerifyingKey>,
    fetched: Option<u64>,
    attempt: Option<u64>,
}

pub struct Verifier {
    up: Arc<dyn Upstream>,
    account_url: String,
    audience: String,
    keys: Mutex<KeyCache>,
    profiles: Mutex<HashMap<String, (u64, Vec<Profile>)>>,
}

impl Verifier {
    pub fn new(up: Arc<dyn Upstream>, account_url: &str) -> Verifier {
        Verifier { up, account_url: account_url.trim_end_matches('/').to_string(), audience: "napple".into(), keys: Mutex::new(KeyCache::default()), profiles: Mutex::new(HashMap::new()) }
    }

    fn refresh(&self, now: u64) -> bool {
        self.keys.lock().unwrap().attempt = Some(now);
        let url = format!("{}/account/v1/.well-known/jwks.json", self.account_url);
        match self.up.request("GET", &url, None, &[], None) {
            Ok((200, body)) => match parse_jwks(&body) {
                Some(k) => {
                    let mut c = self.keys.lock().unwrap();
                    c.keys = k;
                    c.fetched = Some(now);
                    true
                }
                None => false,
            },
            _ => false,
        }
    }

    fn key_for(&self, kid: &str, now: u64) -> Result<VerifyingKey, AuthError> {
        let (known, fresh, ever, attempt) = {
            let c = self.keys.lock().unwrap();
            (c.keys.get(kid).copied(), c.fetched.is_some_and(|t| now < t + JWKS_TTL), c.fetched.is_some(), c.attempt)
        };
        if let (Some(k), true) = (known, fresh) {
            return Ok(k);
        }
        if ever && attempt.is_some_and(|t| now < t + JWKS_MIN_REFRESH) {
            return known.ok_or(AuthError::UnknownKey);
        }
        if self.refresh(now) {
            // the fresh set is authoritative: a key no longer published is revoked
            return self.keys.lock().unwrap().keys.get(kid).copied().ok_or(AuthError::UnknownKey);
        }
        known.ok_or(AuthError::Unavailable)
    }

    pub fn verify(&self, token: &str, now: u64) -> Result<Claims, AuthError> {
        let (kid, signing, sig, claims) = parse_token(token)?;
        let key = self.key_for(&kid, now)?;
        let sig = Signature::from_bytes(&<[u8; 64]>::try_from(sig).map_err(|_| AuthError::Malformed)?);
        key.verify_strict(signing.as_bytes(), &sig).map_err(|_| AuthError::BadSignature)?;
        check_claims(&claims, &self.audience, now)
    }

    fn fetch_profiles(&self, token: &str) -> Result<Vec<Profile>, AuthError> {
        let url = format!("{}/account/v1/profiles", self.account_url);
        let (st, body) = self.up.request("GET", &url, Some(token.trim_end_matches('\0')), &[], None).map_err(|_| AuthError::Unavailable)?;
        if st != 200 {
            return Err(if st == 401 { AuthError::Malformed } else { AuthError::Unavailable });
        }
        let v: Value = serde_json::from_slice(&body).map_err(|_| AuthError::Unavailable)?;
        let items = v.get("profiles").and_then(|p| p.as_array()).or_else(|| v.as_array()).ok_or(AuthError::Unavailable)?;
        Ok(items
            .iter()
            .filter_map(|it| {
                let id = it.get("profile_id").or_else(|| it.get("id"))?.as_str().filter(|s| is_uuid(s))?;
                Some(Profile {
                    profile_id: id.to_ascii_lowercase(),
                    nickname: it.get("nickname").and_then(|n| n.as_str()).unwrap_or("").to_string(),
                    tier: it.get("tier").and_then(|t| t.as_u64()).filter(|t| (1..=5).contains(t)).unwrap_or(1) as u8,
                })
            })
            .collect())
    }

    /// The acting profile `want` (or the account's first profile when None) if it belongs to the token's account.
    pub fn profile(&self, token: &str, claims: &Claims, want: Option<&str>, now: u64) -> Result<Profile, AuthError> {
        let want = want.map(|w| w.to_ascii_lowercase());
        let pick = |list: &[Profile]| match &want {
            Some(w) => list.iter().find(|p| &p.profile_id == w).cloned(),
            None => list.first().cloned(),
        };
        let cached = self.profiles.lock().unwrap().get(&claims.account_id).filter(|(t, _)| now < t + PROFILE_TTL).cloned();
        if let Some((t, list)) = cached {
            if let Some(p) = pick(&list) {
                return Ok(p);
            }
            if now < t + PROFILE_MISS_REFETCH {
                return Err(AuthError::NoProfile);
            }
        }
        let list = self.fetch_profiles(token)?;
        let found = pick(&list);
        self.profiles.lock().unwrap().insert(claims.account_id.clone(), (now, list));
        found.ok_or(AuthError::NoProfile)
    }
}

#[cfg(test)]
pub mod testutil {
    use super::*;
    use crate::crypto::base64;
    use ed25519_dalek::{Signer, SigningKey};
    use std::sync::atomic::{AtomicUsize, Ordering};

    pub const ACC: &str = "0192f2a0-0000-7000-8000-000000000001";
    pub const PROF: &str = "0192f2a0-0000-7000-8000-0000000000a1";

    pub fn key(seed: u8) -> SigningKey {
        SigningKey::from_bytes(&[seed; 32])
    }
    pub fn mint(key: &SigningKey, kid: &str, claims: Value) -> String {
        let h = base64(serde_json::to_vec(&serde_json::json!({"alg": "EdDSA", "typ": "JWT", "kid": kid})).unwrap().as_slice(), true);
        let p = base64(serde_json::to_vec(&claims).unwrap().as_slice(), true);
        let sig = key.sign(format!("{h}.{p}").as_bytes());
        format!("{h}.{p}.{}", base64(&sig.to_bytes(), true))
    }
    pub fn good_claims(now: u64) -> Value {
        serde_json::json!({"sub": ACC, "did": "dev1", "aud": "napple", "exp": now + 900, "iat": now})
    }

    /// Mock account (JWKS + profiles) and social (records posts).
    #[derive(Default)]
    pub struct Mock {
        pub jwks_keys: Mutex<Vec<(String, [u8; 32])>>,
        pub profiles: Mutex<Vec<(String, String, u8)>>,
        pub jwks_calls: AtomicUsize,
        pub profile_calls: AtomicUsize,
        pub posts: Mutex<Vec<(String, Option<String>, Vec<(String, String)>, Value)>>,
        pub social_status: Mutex<u16>,
    }
    impl Mock {
        pub fn with_key(kid: &str, k: &SigningKey) -> Arc<Mock> {
            let m = Mock::default();
            m.jwks_keys.lock().unwrap().push((kid.into(), k.verifying_key().to_bytes()));
            m.profiles.lock().unwrap().push((PROF.into(), "Mario_".into(), 1));
            *m.social_status.lock().unwrap() = 201;
            Arc::new(m)
        }
    }
    impl Upstream for Mock {
        fn request(&self, method: &str, url: &str, bearer: Option<&str>, headers: &[(&str, &str)], body: Option<&str>) -> Result<(u16, Vec<u8>), String> {
            if url.ends_with("/jwks.json") {
                self.jwks_calls.fetch_add(1, Ordering::SeqCst);
                let keys: Vec<Value> = self.jwks_keys.lock().unwrap().iter().map(|(kid, x)| serde_json::json!({"kty":"OKP","crv":"Ed25519","use":"sig","kid":kid,"x":base64(x,true)})).collect();
                return Ok((200, serde_json::to_vec(&serde_json::json!({"keys": keys})).unwrap()));
            }
            if url.ends_with("/account/v1/profiles") {
                self.profile_calls.fetch_add(1, Ordering::SeqCst);
                if bearer.is_none() {
                    return Ok((401, vec![]));
                }
                let ps: Vec<Value> = self.profiles.lock().unwrap().iter().map(|(i, n, t)| serde_json::json!({"profile_id": i, "nickname": n, "tier": t})).collect();
                return Ok((200, serde_json::to_vec(&serde_json::json!({"profiles": ps})).unwrap()));
            }
            if method == "POST" {
                let st = *self.social_status.lock().unwrap();
                self.posts.lock().unwrap().push((
                    url.to_string(),
                    bearer.map(|b| b.to_string()),
                    headers.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect(),
                    serde_json::from_str(body.unwrap_or("null")).unwrap_or(Value::Null),
                ));
                return Ok((st, b"{}".to_vec()));
            }
            Err("no route".into())
        }
    }
}

#[cfg(test)]
mod tests {
    use super::testutil::*;
    use super::*;
    use serde_json::json;
    use std::sync::atomic::Ordering;

    const NOW: u64 = 1_800_000_000;

    fn setup() -> (Arc<Mock>, Verifier, ed25519_dalek::SigningKey) {
        let k = key(7);
        let m = Mock::with_key("k1", &k);
        (m.clone(), Verifier::new(m, "http://acct/"), k)
    }

    #[test]
    fn principal_is_deterministic_and_distinct() {
        let a = principal_id(PROF);
        assert_eq!(a, principal_id(&PROF.to_ascii_uppercase()));
        assert_ne!(a, principal_id(ACC));
        assert!(is_principal(a));
        assert!(!is_principal(crate::accounts::FIRST_PID + 5));
    }

    #[test]
    fn verifies_and_checks_claims() {
        let (m, v, k) = setup();
        let c = v.verify(&mint(&k, "k1", good_claims(NOW)), NOW).unwrap();
        assert_eq!((c.account_id.as_str(), c.device_id.as_str()), (ACC, "dev1"));
        // a NUL-terminated NEX string
        v.verify(&format!("{}\0", mint(&k, "k1", good_claims(NOW))), NOW).unwrap();
        assert_eq!(m.jwks_calls.load(Ordering::SeqCst), 1, "the JWKS is cached");
        let mut e = good_claims(NOW);
        e["exp"] = json!(NOW - 31);
        assert_eq!(v.verify(&mint(&k, "k1", e), NOW), Err(AuthError::Expired));
        let mut e = good_claims(NOW);
        e["exp"] = json!(NOW - 5);
        v.verify(&mint(&k, "k1", e), NOW).unwrap();
        let mut a = good_claims(NOW);
        a["aud"] = json!("nintendo");
        assert_eq!(v.verify(&mint(&k, "k1", a), NOW), Err(AuthError::WrongAudience));
        let mut s = good_claims(NOW);
        s["sub"] = json!("nope");
        assert_eq!(v.verify(&mint(&k, "k1", s), NOW), Err(AuthError::Malformed));
        // another key's signature under the same kid
        assert_eq!(v.verify(&mint(&key(9), "k1", good_claims(NOW)), NOW), Err(AuthError::BadSignature));
        // legacy HS256 token shape
        assert!(!looks_like_account_jwt("eyJhbGciOiJIUzI1NiJ9.e30.AAAA"));
    }

    #[test]
    fn unknown_kid_refetch_is_rate_limited_and_rotation_works() {
        let (m, v, k) = setup();
        v.verify(&mint(&k, "k1", good_claims(NOW)), NOW).unwrap();
        let k2 = key(8);
        let t2 = mint(&k2, "k2", good_claims(NOW + 3));
        assert_eq!(v.verify(&t2, NOW + 3), Err(AuthError::UnknownKey), "inside the 10 s window: no refetch");
        assert_eq!(m.jwks_calls.load(Ordering::SeqCst), 1);
        m.jwks_keys.lock().unwrap().push(("k2".into(), k2.verifying_key().to_bytes()));
        v.verify(&mint(&k2, "k2", good_claims(NOW + 20)), NOW + 20).unwrap();
        assert_eq!(m.jwks_calls.load(Ordering::SeqCst), 2);
        // revocation: k1 is no longer published, found out at the next refresh
        m.jwks_keys.lock().unwrap().retain(|(k, _)| k == "k2");
        assert_eq!(v.verify(&mint(&k, "k1", good_claims(NOW + JWKS_TTL + 30)), NOW + JWKS_TTL + 30), Err(AuthError::UnknownKey));
    }

    #[test]
    fn profile_must_belong_to_the_account_and_is_cached() {
        let (m, v, k) = setup();
        let tok = mint(&k, "k1", good_claims(NOW));
        let c = v.verify(&tok, NOW).unwrap();
        let p = v.profile(&tok, &c, None, NOW).unwrap();
        assert_eq!((p.profile_id.as_str(), p.nickname.as_str(), p.tier), (PROF, "Mario_", 1));
        v.profile(&tok, &c, Some(PROF), NOW + 1).unwrap();
        assert_eq!(m.profile_calls.load(Ordering::SeqCst), 1);
        let other = "0192f2a0-0000-7000-8000-0000000000b2";
        assert_eq!(v.profile(&tok, &c, Some(other), NOW + 2), Err(AuthError::NoProfile));
        assert_eq!(m.profile_calls.load(Ordering::SeqCst), 1, "a recent miss is not refetched");
        // a profile created on another device shows up after the miss window
        m.profiles.lock().unwrap().push((other.into(), "Luigi_".into(), 5));
        let p = v.profile(&tok, &c, Some(other), NOW + 10).unwrap();
        assert_eq!(p.tier, 5);
    }
}
