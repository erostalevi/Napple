//! Ed25519 (EdDSA) JWT access tokens and the JWKS napple-online verifies them with.
//!
//! Header `{"alg":"EdDSA","typ":"JWT","kid":...}`; claims `iss`, `aud`="napple", `sub`=account_id, `did`=device_id,
//! `sep` (device session epoch; logout/revoke/reuse bump it), `prf` (the account's profile ids), `iat`, `exp`, `jti`.
//! Keys live in `<data>/jwt-keys.json` (0600). The newest key signs; every key in the file is published, so a rotation
//! (`napple-account rotate-jwt-key`) keeps tokens signed by the previous key valid until they expire.

use crate::util::{b64_decode, b64url, random_bytes, sha256};
use ed25519_dalek::{Signature, Signer, SigningKey, Verifier, VerifyingKey};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::path::Path;

pub const AUDIENCE: &str = "napple";
/// Keys kept in the file (and JWKS) after a rotation.
const KEEP_KEYS: usize = 3;

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct Claims {
    pub iss: String,
    pub aud: String,
    pub sub: String,
    pub did: String,
    pub sep: i64,
    #[serde(default)]
    pub prf: Vec<String>,
    pub iat: i64,
    pub exp: i64,
    pub jti: String,
}

struct Key {
    kid: String,
    signing: SigningKey,
}

pub struct JwtKeys {
    keys: Vec<Key>, // newest first
}

#[derive(Serialize, Deserialize)]
struct KeyFileEntry {
    kid: String,
    seed: String,
    created_at: i64,
}

fn kid_for(vk: &VerifyingKey) -> String {
    b64url(&sha256(vk.as_bytes())[..12])
}

impl JwtKeys {
    pub fn from_seed(seed: [u8; 32]) -> JwtKeys {
        let signing = SigningKey::from_bytes(&seed);
        JwtKeys {
            keys: vec![Key {
                kid: kid_for(&signing.verifying_key()),
                signing,
            }],
        }
    }

    pub fn generate() -> JwtKeys {
        JwtKeys::from_seed(random_bytes::<32>())
    }

    /// Loads `<dir>/jwt-keys.json`, creating it with a fresh key if missing.
    pub fn load_or_create(dir: &Path, now: i64) -> Result<JwtKeys, String> {
        let path = dir.join("jwt-keys.json");
        if !path.exists() {
            write_key_file(&path, &[KeyFileEntry::fresh(now)])?;
        }
        let entries: Vec<KeyFileEntry> = serde_json::from_slice(
            &std::fs::read(&path).map_err(|e| format!("{}: {e}", path.display()))?,
        )
        .map_err(|e| format!("{}: {e}", path.display()))?;
        let mut keys = Vec::new();
        for e in entries {
            let seed: [u8; 32] = b64_decode(&e.seed)
                .and_then(|v| v.try_into().ok())
                .ok_or("jwt-keys.json: bad seed")?;
            let signing = SigningKey::from_bytes(&seed);
            keys.push(Key {
                kid: kid_for(&signing.verifying_key()),
                signing,
            });
        }
        if keys.is_empty() {
            return Err("jwt-keys.json has no keys".into());
        }
        Ok(JwtKeys { keys })
    }

    /// Adds a new signing key at the front of the key file (keeps the newest KEEP_KEYS). Returns the new kid.
    pub fn rotate(dir: &Path, now: i64) -> Result<String, String> {
        let path = dir.join("jwt-keys.json");
        let mut entries: Vec<KeyFileEntry> = match std::fs::read(&path) {
            Ok(b) => serde_json::from_slice(&b).map_err(|e| e.to_string())?,
            Err(_) => Vec::new(),
        };
        let fresh = KeyFileEntry::fresh(now);
        let kid = fresh.kid.clone();
        entries.insert(0, fresh);
        entries.truncate(KEEP_KEYS);
        write_key_file(&path, &entries)?;
        Ok(kid)
    }

    pub fn current_kid(&self) -> &str {
        &self.keys[0].kid
    }

    pub fn sign(&self, claims: &Claims) -> String {
        let k = &self.keys[0];
        let header = json!({"alg": "EdDSA", "typ": "JWT", "kid": k.kid});
        let signing_input = format!(
            "{}.{}",
            b64url(header.to_string().as_bytes()),
            b64url(serde_json::to_string(claims).unwrap().as_bytes())
        );
        let sig = k.signing.sign(signing_input.as_bytes());
        format!("{signing_input}.{}", b64url(&sig.to_bytes()))
    }

    /// Verifies signature, `kid`, `alg`, `aud`, `iss` and lifetime (with 30 s leeway on `iat`).
    pub fn verify(&self, token: &str, issuer: &str, now: i64) -> Result<Claims, &'static str> {
        let mut parts = token.split('.');
        let (Some(h), Some(p), Some(s), None) =
            (parts.next(), parts.next(), parts.next(), parts.next())
        else {
            return Err("malformed token");
        };
        let header: Value = serde_json::from_slice(&b64_decode(h).ok_or("bad header")?)
            .map_err(|_| "bad header")?;
        if header.get("alg").and_then(Value::as_str) != Some("EdDSA") {
            return Err("unsupported alg");
        }
        let kid = header
            .get("kid")
            .and_then(Value::as_str)
            .ok_or("missing kid")?;
        let key = self
            .keys
            .iter()
            .find(|k| k.kid == kid)
            .ok_or("unknown kid")?;
        let sig_bytes: [u8; 64] = b64_decode(s)
            .and_then(|v| v.try_into().ok())
            .ok_or("bad signature")?;
        key.signing
            .verifying_key()
            .verify(
                format!("{h}.{p}").as_bytes(),
                &Signature::from_bytes(&sig_bytes),
            )
            .map_err(|_| "bad signature")?;
        let claims: Claims = serde_json::from_slice(&b64_decode(p).ok_or("bad claims")?)
            .map_err(|_| "bad claims")?;
        if claims.aud != AUDIENCE || claims.iss != issuer {
            return Err("wrong audience or issuer");
        }
        if now >= claims.exp || claims.iat > now + 30 {
            return Err("token expired");
        }
        Ok(claims)
    }

    /// RFC 8037 OKP keys.
    pub fn jwks(&self) -> Value {
        let keys: Vec<Value> = self
            .keys
            .iter()
            .map(|k| json!({"kty": "OKP", "crv": "Ed25519", "x": b64url(k.signing.verifying_key().as_bytes()), "kid": k.kid, "use": "sig", "alg": "EdDSA"}))
            .collect();
        json!({ "keys": keys })
    }
}

impl KeyFileEntry {
    fn fresh(now: i64) -> KeyFileEntry {
        let seed = random_bytes::<32>();
        let signing = SigningKey::from_bytes(&seed);
        KeyFileEntry {
            kid: kid_for(&signing.verifying_key()),
            seed: b64url(&seed),
            created_at: now,
        }
    }
}

fn write_key_file(path: &Path, entries: &[KeyFileEntry]) -> Result<(), String> {
    use std::io::Write;
    let tmp = path.with_extension("json.tmp");
    let mut opts = std::fs::OpenOptions::new();
    opts.write(true).create(true).truncate(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        opts.mode(0o600);
    }
    let mut f = opts
        .open(&tmp)
        .map_err(|e| format!("{}: {e}", tmp.display()))?;
    f.write_all(&serde_json::to_vec_pretty(entries).unwrap())
        .map_err(|e| e.to_string())?;
    f.sync_all().map_err(|e| e.to_string())?;
    std::fs::rename(&tmp, path).map_err(|e| e.to_string())
}

/// Verifies a token against a JWKS document alone (what napple-online does after fetching
/// `/account/v1/.well-known/jwks.json`). Kept here as the reference implementation and tested against `JwtKeys::jwks`.
pub fn verify_with_jwks(
    jwks: &Value,
    token: &str,
    issuer: &str,
    now: i64,
) -> Result<Claims, &'static str> {
    let (signing_input, sig) = token.rsplit_once('.').ok_or("malformed token")?;
    let (h, p) = signing_input.split_once('.').ok_or("malformed token")?;
    let header: Value =
        serde_json::from_slice(&b64_decode(h).ok_or("bad header")?).map_err(|_| "bad header")?;
    if header.get("alg").and_then(Value::as_str) != Some("EdDSA") {
        return Err("unsupported alg");
    }
    let kid = header
        .get("kid")
        .and_then(Value::as_str)
        .ok_or("missing kid")?;
    let jwk = jwks["keys"]
        .as_array()
        .and_then(|ks| ks.iter().find(|k| k["kid"] == kid))
        .ok_or("unknown kid")?;
    if jwk["kty"] != "OKP" || jwk["crv"] != "Ed25519" {
        return Err("unsupported key");
    }
    let x: [u8; 32] = jwk["x"]
        .as_str()
        .and_then(b64_decode)
        .and_then(|v| v.try_into().ok())
        .ok_or("bad key")?;
    let vk = VerifyingKey::from_bytes(&x).map_err(|_| "bad key")?;
    let sig: [u8; 64] = b64_decode(sig)
        .and_then(|v| v.try_into().ok())
        .ok_or("bad signature")?;
    vk.verify_strict(signing_input.as_bytes(), &Signature::from_bytes(&sig))
        .map_err(|_| "bad signature")?;
    let claims: Claims =
        serde_json::from_slice(&b64_decode(p).ok_or("bad claims")?).map_err(|_| "bad claims")?;
    if claims.aud != AUDIENCE || claims.iss != issuer || now >= claims.exp {
        return Err("expired or wrong audience/issuer");
    }
    Ok(claims)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn claims(now: i64) -> Claims {
        Claims {
            iss: "https://x/account/v1".into(),
            aud: AUDIENCE.into(),
            sub: "acc".into(),
            did: "dev".into(),
            sep: 0,
            prf: vec!["p1".into()],
            iat: now,
            exp: now + 900,
            jti: "j".into(),
        }
    }

    #[test]
    fn sign_verify_and_jwks() {
        let k = JwtKeys::generate();
        let t = k.sign(&claims(1000));
        assert_eq!(
            k.verify(&t, "https://x/account/v1", 1001).unwrap(),
            claims(1000)
        );
        assert_eq!(
            verify_with_jwks(&k.jwks(), &t, "https://x/account/v1", 1001)
                .unwrap()
                .sub,
            "acc"
        );
        assert!(
            k.verify(&t, "https://x/account/v1", 1900).is_err(),
            "expired"
        );
        assert!(k.verify(&t, "https://other", 1001).is_err(), "issuer");
        // Tampered payload.
        let mut parts: Vec<&str> = t.split('.').collect();
        let forged = b64url(
            serde_json::to_string(&Claims {
                sub: "victim".into(),
                ..claims(1000)
            })
            .unwrap()
            .as_bytes(),
        );
        parts[1] = &forged;
        assert!(
            k.verify(&parts.join("."), "https://x/account/v1", 1001)
                .is_err()
        );
        // alg confusion.
        let none = format!(
            "{}.{}.",
            b64url(br#"{"alg":"none","kid":"x"}"#),
            t.split('.').nth(1).unwrap()
        );
        assert!(k.verify(&none, "https://x/account/v1", 1001).is_err());
        // Another server's key.
        assert!(
            JwtKeys::generate()
                .verify(&t, "https://x/account/v1", 1001)
                .is_err()
        );
    }

    #[test]
    fn rotation_keeps_old_tokens_valid() {
        let dir =
            std::env::temp_dir().join(format!("napple-account-jwt-{}", crate::util::new_id()));
        std::fs::create_dir_all(&dir).unwrap();
        let k1 = JwtKeys::load_or_create(&dir, 1).unwrap();
        let t1 = k1.sign(&claims(1000));
        let new_kid = JwtKeys::rotate(&dir, 2).unwrap();
        let k2 = JwtKeys::load_or_create(&dir, 3).unwrap();
        assert_eq!(k2.current_kid(), new_kid);
        assert!(k2.verify(&t1, "https://x/account/v1", 1001).is_ok());
        assert_eq!(k2.jwks()["keys"].as_array().unwrap().len(), 2);
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(
                std::fs::metadata(dir.join("jwt-keys.json"))
                    .unwrap()
                    .permissions()
                    .mode()
                    & 0o777,
                0o600
            );
        }
        std::fs::remove_dir_all(dir).ok();
    }
}
