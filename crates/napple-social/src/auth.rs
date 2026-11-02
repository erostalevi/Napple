//! Authentication against the Napple Account service.
//!
//! * Access tokens are compact JWS (JWT) signed with Ed25519 (`alg` "EdDSA", or the fully specified "Ed25519"), with a `kid`.
//!   Claims: `sub` = account id (UUID), `did` = device id, `aud` = "napple", `exp` (15 min). Keys come from the account
//!   service's JWKS (OKP / Ed25519), cached, re-fetched on age and, rate-limited, on an unknown `kid`.
//! * The acting profile (header `X-Napple-Profile`) must be one of the account's profiles, as listed by
//!   `GET {account}/account/v1/profiles` called with the caller's own token; the list is cached per account for 60 s.

use std::collections::HashMap;
use std::sync::Mutex;
use std::time::Duration;

use base64::Engine;
use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use ed25519_dalek::{Signature, VerifyingKey};
use serde::Deserialize;
use serde_json::Value;
use uuid::Uuid;

use crate::config::{
    Config, JWKS_MIN_REFRESH_MS, JWKS_TTL_MS, PROFILE_CACHE_MS, PROFILE_MISS_REFETCH_MS,
    TOKEN_LEEWAY_SECS,
};
use crate::error::ApiError;

/// Verified token claims.
#[derive(Debug, Clone)]
pub struct Claims {
    /// Account id, canonical lowercase hyphenated UUID.
    pub account_id: String,
    pub device_id: String,
    /// Expiry, unix seconds.
    pub exp: i64,
}

/// A profile as the account service reports it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProfileInfo {
    pub profile_id: String,
    pub nickname: String,
    pub icon_id: String,
}

#[derive(Deserialize)]
struct Header {
    alg: String,
    kid: Option<String>,
}

#[derive(Deserialize)]
#[serde(untagged)]
enum Aud {
    One(String),
    Many(Vec<String>),
}

#[derive(Deserialize)]
struct RawClaims {
    sub: Option<String>,
    did: Option<String>,
    aud: Option<Aud>,
    exp: Option<i64>,
    nbf: Option<i64>,
}

#[derive(Deserialize)]
struct JwkSet {
    keys: Vec<Jwk>,
}

#[derive(Deserialize)]
struct Jwk {
    kty: String,
    crv: Option<String>,
    kid: Option<String>,
    x: Option<String>,
    #[serde(rename = "use")]
    use_: Option<String>,
}

/// Parses a JWKS document into `kid -> key`, skipping keys that are not Ed25519 signing keys.
pub fn parse_jwks(doc: &[u8]) -> Result<HashMap<String, VerifyingKey>, String> {
    let set: JwkSet = serde_json::from_slice(doc).map_err(|e| format!("jwks: {e}"))?;
    let mut out = HashMap::new();
    for k in set.keys {
        if k.kty != "OKP"
            || k.crv.as_deref() != Some("Ed25519")
            || k.use_.as_deref().is_some_and(|u| u != "sig")
        {
            continue;
        }
        let (Some(kid), Some(x)) = (k.kid, k.x) else {
            continue;
        };
        let Ok(raw) = URL_SAFE_NO_PAD.decode(x.trim_end_matches('=')) else {
            continue;
        };
        let Ok(raw): Result<[u8; 32], _> = raw.try_into() else {
            continue;
        };
        if let Ok(vk) = VerifyingKey::from_bytes(&raw) {
            out.insert(kid, vk);
        }
    }
    Ok(out)
}

fn b64(part: &str) -> Result<Vec<u8>, ApiError> {
    URL_SAFE_NO_PAD
        .decode(part.trim_end_matches('='))
        .map_err(|_| ApiError::unauthorized("invalid_token", "malformed token"))
}

/// Header-only peek, used to find the key.
fn token_kid(token: &str) -> Result<String, ApiError> {
    let mut parts = token.split('.');
    let (Some(h), Some(_), Some(_), None) =
        (parts.next(), parts.next(), parts.next(), parts.next())
    else {
        return Err(ApiError::unauthorized("invalid_token", "malformed token"));
    };
    let header: Header = serde_json::from_slice(&b64(h)?)
        .map_err(|_| ApiError::unauthorized("invalid_token", "malformed token header"))?;
    if header.alg != "EdDSA" && header.alg != "Ed25519" {
        return Err(ApiError::unauthorized("invalid_token", "unsupported alg"));
    }
    header
        .kid
        .filter(|k| !k.is_empty())
        .ok_or_else(|| ApiError::unauthorized("invalid_token", "token has no kid"))
}

/// Verifies signature and claims with a known key. `now_ms` is the service clock.
pub fn verify_with_key(
    token: &str,
    key: &VerifyingKey,
    audience: &str,
    now_ms: i64,
) -> Result<Claims, ApiError> {
    token_kid(token)?;
    let mut parts = token.splitn(3, '.');
    let (h, p, s) = (
        parts.next().unwrap_or(""),
        parts.next().unwrap_or(""),
        parts.next().unwrap_or(""),
    );
    let sig: [u8; 64] = b64(s)?
        .try_into()
        .map_err(|_| ApiError::unauthorized("invalid_token", "bad signature length"))?;
    let signing_input = &token[..h.len() + 1 + p.len()];
    key.verify_strict(signing_input.as_bytes(), &Signature::from_bytes(&sig))
        .map_err(|_| ApiError::unauthorized("bad_signature", "token signature does not verify"))?;
    let claims: RawClaims = serde_json::from_slice(&b64(p)?)
        .map_err(|_| ApiError::unauthorized("invalid_token", "malformed token claims"))?;
    let now_s = now_ms.div_euclid(1000);
    let exp = claims
        .exp
        .ok_or_else(|| ApiError::unauthorized("invalid_token", "token has no exp"))?;
    if now_s >= exp + TOKEN_LEEWAY_SECS {
        return Err(ApiError::unauthorized("token_expired", "token expired"));
    }
    if let Some(nbf) = claims.nbf
        && now_s + TOKEN_LEEWAY_SECS < nbf
    {
        return Err(ApiError::unauthorized(
            "invalid_token",
            "token not yet valid",
        ));
    }
    let aud_ok = match &claims.aud {
        Some(Aud::One(a)) => a == audience,
        Some(Aud::Many(v)) => v.iter().any(|a| a == audience),
        None => false,
    };
    if !aud_ok {
        return Err(ApiError::unauthorized(
            "wrong_audience",
            "token audience is not napple",
        ));
    }
    let sub = claims
        .sub
        .ok_or_else(|| ApiError::unauthorized("invalid_token", "token has no sub"))?;
    let account = Uuid::parse_str(&sub)
        .map_err(|_| ApiError::unauthorized("invalid_token", "sub is not a UUID"))?;
    let device_id = claims
        .did
        .filter(|d| !d.is_empty())
        .ok_or_else(|| ApiError::unauthorized("invalid_token", "token has no did"))?;
    Ok(Claims {
        account_id: account.hyphenated().to_string(),
        device_id,
        exp,
    })
}

struct KeyCache {
    keys: HashMap<String, VerifyingKey>,
    fetched_at_ms: Option<i64>,
    last_attempt_ms: Option<i64>,
}

struct CachedProfiles {
    fetched_at_ms: i64,
    profiles: Vec<ProfileInfo>,
}

pub struct Auth {
    http: reqwest::Client,
    jwks_url: String,
    profiles_url: String,
    audience: String,
    keys: Mutex<KeyCache>,
    refresh_lock: tokio::sync::Mutex<()>,
    profiles: Mutex<HashMap<String, CachedProfiles>>,
}

impl Auth {
    pub fn new(config: &Config) -> Self {
        let http = reqwest::Client::builder()
            .timeout(Duration::from_secs(5))
            .connect_timeout(Duration::from_secs(3))
            .user_agent(concat!("napple-social/", env!("CARGO_PKG_VERSION")))
            .build()
            .expect("http client");
        Auth {
            http,
            jwks_url: config.jwks_url(),
            profiles_url: config.profiles_url(),
            audience: config.audience.clone(),
            keys: Mutex::new(KeyCache {
                keys: HashMap::new(),
                fetched_at_ms: None,
                last_attempt_ms: None,
            }),
            refresh_lock: tokio::sync::Mutex::new(()),
            profiles: Mutex::new(HashMap::new()),
        }
    }

    fn cached_key(&self, kid: &str) -> (Option<VerifyingKey>, bool, Option<i64>) {
        let c = self.keys.lock().unwrap_or_else(|e| e.into_inner());
        (
            c.keys.get(kid).copied(),
            c.fetched_at_ms.is_some(),
            c.last_attempt_ms,
        )
    }

    async fn refresh_keys(&self, now_ms: i64) -> Result<(), ApiError> {
        {
            let mut c = self.keys.lock().unwrap_or_else(|e| e.into_inner());
            c.last_attempt_ms = Some(now_ms);
        }
        let resp = self.http.get(&self.jwks_url).send().await.map_err(|e| {
            tracing::warn!(error = %e, "jwks fetch failed");
            ApiError::unavailable(
                "account_unavailable",
                "cannot reach the Napple Account service",
            )
        })?;
        if !resp.status().is_success() {
            tracing::warn!(status = %resp.status(), "jwks fetch failed");
            return Err(ApiError::unavailable(
                "account_unavailable",
                "Napple Account service returned an error",
            ));
        }
        let body = resp
            .bytes()
            .await
            .map_err(|_| ApiError::unavailable("account_unavailable", "jwks read failed"))?;
        let keys = parse_jwks(&body).map_err(|e| {
            tracing::warn!(error = %e, "jwks parse failed");
            ApiError::unavailable(
                "account_unavailable",
                "Napple Account service returned a bad JWKS",
            )
        })?;
        tracing::info!(keys = keys.len(), "jwks refreshed");
        let mut c = self.keys.lock().unwrap_or_else(|e| e.into_inner());
        c.keys = keys;
        c.fetched_at_ms = Some(now_ms);
        Ok(())
    }

    async fn key_for(&self, kid: &str, now_ms: i64) -> Result<VerifyingKey, ApiError> {
        let fresh = {
            let c = self.keys.lock().unwrap_or_else(|e| e.into_inner());
            c.fetched_at_ms.is_some_and(|t| now_ms - t < JWKS_TTL_MS)
        };
        let (key, _, _) = self.cached_key(kid);
        if let (Some(k), true) = (key, fresh) {
            return Ok(k);
        }
        let _guard = self.refresh_lock.lock().await;
        // Someone else may have refreshed while we waited.
        let (key, ever, last_attempt) = self.cached_key(kid);
        let fresh = {
            let c = self.keys.lock().unwrap_or_else(|e| e.into_inner());
            c.fetched_at_ms.is_some_and(|t| now_ms - t < JWKS_TTL_MS)
        };
        if let (Some(k), true) = (key, fresh) {
            return Ok(k);
        }
        let unknown = || ApiError::unauthorized("unknown_kid", "token signed with an unknown key");
        let throttled = last_attempt.is_some_and(|t| now_ms - t < JWKS_MIN_REFRESH_MS) && ever;
        if throttled {
            // A stale key is still better than nothing until the next refresh is allowed.
            return key.ok_or_else(unknown);
        }
        match self.refresh_keys(now_ms).await {
            // The fresh set is authoritative: a key the account service no longer publishes is revoked.
            Ok(()) => self.cached_key(kid).0.ok_or_else(unknown),
            // Stale but known key: keep working through an account-service outage.
            Err(e) => key.ok_or(e),
        }
    }

    /// Full token verification (signature via JWKS + claims).
    pub async fn verify(&self, token: &str, now_ms: i64) -> Result<Claims, ApiError> {
        let kid = token_kid(token)?;
        let key = self.key_for(&kid, now_ms).await?;
        verify_with_key(token, &key, &self.audience, now_ms)
    }

    async fn fetch_profiles(&self, token: &str) -> Result<Vec<ProfileInfo>, ApiError> {
        let resp = self
            .http
            .get(&self.profiles_url)
            .bearer_auth(token)
            .send()
            .await
            .map_err(|e| {
                tracing::warn!(error = %e, "profiles fetch failed");
                ApiError::unavailable(
                    "account_unavailable",
                    "cannot reach the Napple Account service",
                )
            })?;
        match resp.status().as_u16() {
            200 => {}
            401 => {
                return Err(ApiError::unauthorized(
                    "invalid_token",
                    "Napple Account service rejected the token",
                ));
            }
            s => {
                tracing::warn!(status = s, "profiles fetch failed");
                return Err(ApiError::unavailable(
                    "account_unavailable",
                    "Napple Account service returned an error",
                ));
            }
        }
        let v: Value = resp
            .json()
            .await
            .map_err(|_| ApiError::unavailable("account_unavailable", "bad profiles response"))?;
        Ok(parse_profiles(&v))
    }

    /// Returns the acting profile if it belongs to the token's account. Cached per account for 60 s; a profile missing
    /// from a cached list (just created on another device) re-fetches once the list is 5 s old.
    pub async fn owned_profile(
        &self,
        token: &str,
        account_id: &str,
        profile_id: &str,
        now_ms: i64,
    ) -> Result<ProfileInfo, ApiError> {
        let cached = {
            let m = self.profiles.lock().unwrap_or_else(|e| e.into_inner());
            m.get(account_id)
                .filter(|c| now_ms - c.fetched_at_ms < PROFILE_CACHE_MS)
                .map(|c| {
                    (
                        c.profiles
                            .iter()
                            .find(|p| p.profile_id == profile_id)
                            .cloned(),
                        now_ms - c.fetched_at_ms,
                    )
                })
        };
        match cached {
            Some((Some(p), _)) => return Ok(p),
            Some((None, age)) if age < PROFILE_MISS_REFETCH_MS => {
                return Err(ApiError::forbidden(
                    "profile_not_owned",
                    "this profile does not belong to your Napple Account",
                ));
            }
            _ => {}
        }
        let profiles = self.fetch_profiles(token).await?;
        let found = profiles
            .iter()
            .find(|p| p.profile_id == profile_id)
            .cloned();
        self.profiles
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .insert(
                account_id.to_string(),
                CachedProfiles {
                    fetched_at_ms: now_ms,
                    profiles,
                },
            );
        found.ok_or_else(|| {
            ApiError::forbidden(
                "profile_not_owned",
                "this profile does not belong to your Napple Account",
            )
        })
    }

    /// Forgets expired ownership lists.
    pub fn prune(&self, now_ms: i64) {
        self.profiles
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .retain(|_, c| now_ms - c.fetched_at_ms < PROFILE_CACHE_MS);
    }
}

/// Accepts `{"profiles": [...]}` or a bare array; each item needs `profile_id` (or `id`) and may carry `nickname`
/// (or `name`) and `icon_id` (or `icon`, string or number). The account service contract is still settling, so be lenient.
pub fn parse_profiles(v: &Value) -> Vec<ProfileInfo> {
    let items = match v {
        Value::Array(a) => a.as_slice(),
        Value::Object(o) => o
            .get("profiles")
            .and_then(Value::as_array)
            .map(Vec::as_slice)
            .unwrap_or(&[]),
        _ => &[],
    };
    let text = |v: Option<&Value>| match v {
        Some(Value::String(s)) => s.clone(),
        Some(Value::Number(n)) => n.to_string(),
        _ => String::new(),
    };
    items
        .iter()
        .filter_map(|it| {
            let id = it
                .get("profile_id")
                .or_else(|| it.get("id"))
                .and_then(Value::as_str)?;
            let id = Uuid::parse_str(id).ok()?.hyphenated().to_string();
            Some(ProfileInfo {
                profile_id: id,
                nickname: text(it.get("nickname").or_else(|| it.get("name"))),
                icon_id: text(it.get("icon_id").or_else(|| it.get("icon"))),
            })
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_dalek::{Signer, SigningKey};
    use serde_json::json;

    fn mint(key: &SigningKey, kid: &str, alg: &str, claims: Value) -> String {
        let h = URL_SAFE_NO_PAD
            .encode(serde_json::to_vec(&json!({"alg": alg, "typ": "JWT", "kid": kid})).unwrap());
        let p = URL_SAFE_NO_PAD.encode(serde_json::to_vec(&claims).unwrap());
        let sig = key.sign(format!("{h}.{p}").as_bytes());
        format!("{h}.{p}.{}", URL_SAFE_NO_PAD.encode(sig.to_bytes()))
    }

    const ACC: &str = "0192f2a0-0000-7000-8000-000000000001";

    #[test]
    fn verify_claims() {
        let key = SigningKey::from_bytes(&[7u8; 32]);
        let vk = key.verifying_key();
        let now = 1_800_000_000_000i64;
        let good = json!({"sub": ACC, "did": "dev1", "aud": "napple", "exp": now / 1000 + 900, "iat": now / 1000});
        let c =
            verify_with_key(&mint(&key, "k1", "EdDSA", good.clone()), &vk, "napple", now).unwrap();
        assert_eq!(c.account_id, ACC);
        assert_eq!(c.device_id, "dev1");
        // Fully specified alg name and array audience.
        let mut arr = good.clone();
        arr["aud"] = json!(["other", "napple"]);
        verify_with_key(&mint(&key, "k1", "Ed25519", arr), &vk, "napple", now).unwrap();

        let code = |claims: Value, alg: &str| {
            verify_with_key(&mint(&key, "k1", alg, claims), &vk, "napple", now)
                .unwrap_err()
                .code
        };
        let mut e = good.clone();
        e["exp"] = json!(now / 1000 - 31);
        assert_eq!(code(e, "EdDSA"), "token_expired");
        let mut e = good.clone();
        e["exp"] = json!(now / 1000 - 5); // inside the leeway
        verify_with_key(&mint(&key, "k1", "EdDSA", e), &vk, "napple", now).unwrap();
        let mut a = good.clone();
        a["aud"] = json!("nintendo");
        assert_eq!(code(a, "EdDSA"), "wrong_audience");
        let mut a = good.clone();
        a.as_object_mut().unwrap().remove("aud");
        assert_eq!(code(a, "EdDSA"), "wrong_audience");
        let mut s = good.clone();
        s["sub"] = json!("not-a-uuid");
        assert_eq!(code(s, "EdDSA"), "invalid_token");
        let mut d = good.clone();
        d.as_object_mut().unwrap().remove("did");
        assert_eq!(code(d, "EdDSA"), "invalid_token");
        let mut n = good.clone();
        n["nbf"] = json!(now / 1000 + 600);
        assert_eq!(code(n, "EdDSA"), "invalid_token");
        assert_eq!(code(good.clone(), "HS256"), "invalid_token");
        assert_eq!(code(good.clone(), "none"), "invalid_token");

        // Another key's signature, and a tampered payload.
        let other = SigningKey::from_bytes(&[9u8; 32]);
        let t = mint(&other, "k1", "EdDSA", good.clone());
        assert_eq!(
            verify_with_key(&t, &vk, "napple", now).unwrap_err().code,
            "bad_signature"
        );
        let t = mint(&key, "k1", "EdDSA", good.clone());
        let mut parts: Vec<String> = t.split('.').map(String::from).collect();
        let mut forged = good.clone();
        forged["sub"] = json!("0192f2a0-0000-7000-8000-000000000002");
        parts[1] = URL_SAFE_NO_PAD.encode(serde_json::to_vec(&forged).unwrap());
        assert_eq!(
            verify_with_key(&parts.join("."), &vk, "napple", now)
                .unwrap_err()
                .code,
            "bad_signature"
        );
        assert_eq!(
            verify_with_key("abc", &vk, "napple", now).unwrap_err().code,
            "invalid_token"
        );
        assert_eq!(
            verify_with_key("a.b.c.d", &vk, "napple", now)
                .unwrap_err()
                .code,
            "invalid_token"
        );
    }

    #[test]
    fn jwks_parsing() {
        let key = SigningKey::from_bytes(&[7u8; 32]);
        let x = URL_SAFE_NO_PAD.encode(key.verifying_key().to_bytes());
        let doc = json!({"keys": [
            {"kty": "OKP", "crv": "Ed25519", "kid": "a", "x": x, "use": "sig", "alg": "EdDSA"},
            {"kty": "OKP", "crv": "X25519", "kid": "b", "x": x},
            {"kty": "RSA", "kid": "c", "n": "xx", "e": "AQAB"},
            {"kty": "OKP", "crv": "Ed25519", "kid": "d", "x": "short"},
            {"kty": "OKP", "crv": "Ed25519", "x": x},
            {"kty": "OKP", "crv": "Ed25519", "kid": "e", "x": x, "use": "enc"}
        ]});
        let keys = parse_jwks(&serde_json::to_vec(&doc).unwrap()).unwrap();
        assert_eq!(keys.len(), 1);
        assert!(keys.contains_key("a"));
        assert!(parse_jwks(b"nope").is_err());
    }

    #[test]
    fn profile_list_shapes() {
        let p = "0192f2a0-0000-7000-8000-0000000000AA";
        let v = parse_profiles(
            &json!({"profiles": [{"profile_id": p, "nickname": "Tal", "icon_id": 3}, {"id": "bad"}]}),
        );
        assert_eq!(
            v,
            vec![ProfileInfo {
                profile_id: p.to_lowercase(),
                nickname: "Tal".into(),
                icon_id: "3".into()
            }]
        );
        let v = parse_profiles(&json!([{"id": p, "name": "X", "icon": "mii-2"}]));
        assert_eq!(v[0].icon_id, "mii-2");
        assert!(parse_profiles(&json!("x")).is_empty());
    }
}
