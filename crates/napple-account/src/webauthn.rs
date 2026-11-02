//! Minimal WebAuthn Level 2 relying party for passkeys: registration with `attestation: "none"` and assertions,
//! ES256 (P-256) and EdDSA (Ed25519) credentials. Pure Rust (no OpenSSL), so the binary needs nothing beyond libc.
//!
//! Checks done (WebAuthn §7.1 / §7.2): clientData `type`, `challenge` (constant time), `origin` in the allowed list,
//! `crossOrigin` not true, rpIdHash, UP and UV flags, attested credential data present and matching `rawId`,
//! COSE key alg in {-7, -8}, assertion signature over `authData || SHA-256(clientDataJSON)`, and the signature counter
//! (a non-zero counter that does not increase is rejected as a possible cloned authenticator).
//! The attestation statement is not verified: we ask for `"none"` and do not need to know the authenticator model.

use crate::util::{b64_decode, ct_eq, sha256};
use ciborium::value::Value as Cbor;
use serde::Deserialize;

pub const ALG_ES256: i64 = -7;
pub const ALG_EDDSA: i64 = -8;

const FLAG_UP: u8 = 0x01;
const FLAG_UV: u8 = 0x04;
const FLAG_BE: u8 = 0x08;
const FLAG_AT: u8 = 0x40;

#[derive(Debug)]
pub struct NewCredential {
    pub cred_id: Vec<u8>,
    pub cose_key: Vec<u8>,
    pub alg: i64,
    pub sign_count: u32,
    pub aaguid: [u8; 16],
    pub backup_eligible: bool,
}

#[derive(Deserialize)]
struct ClientData {
    #[serde(rename = "type")]
    ty: String,
    challenge: String,
    origin: String,
    #[serde(default, rename = "crossOrigin")]
    cross_origin: Option<bool>,
}

struct AuthData<'a> {
    rp_id_hash: &'a [u8],
    flags: u8,
    sign_count: u32,
    attested: Option<(
        /*aaguid*/ [u8; 16],
        /*cred id*/ Vec<u8>,
        /*cose*/ Vec<u8>,
    )>,
}

fn check_client_data(
    raw: &[u8],
    ty: &str,
    challenge: &[u8],
    origins: &[String],
) -> Result<(), &'static str> {
    let cd: ClientData = serde_json::from_slice(raw).map_err(|_| "clientDataJSON is not JSON")?;
    if cd.ty != ty {
        return Err("clientData type mismatch");
    }
    let got = b64_decode(&cd.challenge).ok_or("clientData challenge not base64url")?;
    if !ct_eq(&got, challenge) {
        return Err("challenge mismatch");
    }
    if !origins.iter().any(|o| o == &cd.origin) {
        return Err("origin not allowed");
    }
    if cd.cross_origin == Some(true) {
        return Err("cross-origin ceremony");
    }
    Ok(())
}

fn parse_auth_data(d: &[u8]) -> Result<AuthData<'_>, &'static str> {
    if d.len() < 37 {
        return Err("authenticatorData too short");
    }
    let flags = d[32];
    let sign_count = u32::from_be_bytes(d[33..37].try_into().unwrap());
    let mut attested = None;
    if flags & FLAG_AT != 0 {
        let rest = &d[37..];
        if rest.len() < 18 {
            return Err("attested credential data too short");
        }
        let aaguid: [u8; 16] = rest[..16].try_into().unwrap();
        let id_len = u16::from_be_bytes([rest[16], rest[17]]) as usize;
        if id_len == 0 || id_len > 1023 || rest.len() < 18 + id_len {
            return Err("bad credential id length");
        }
        let cred_id = rest[18..18 + id_len].to_vec();
        let mut cursor = &rest[18 + id_len..];
        let before = cursor.len();
        let _: Cbor = ciborium::de::from_reader(&mut cursor).map_err(|_| "bad COSE key")?;
        let used = before - cursor.len();
        let cose = rest[18 + id_len..18 + id_len + used].to_vec();
        attested = Some((aaguid, cred_id, cose));
    }
    Ok(AuthData {
        rp_id_hash: &d[..32],
        flags,
        sign_count,
        attested,
    })
}

fn check_flags_and_rp(ad: &AuthData, rp_id: &str) -> Result<(), &'static str> {
    if !ct_eq(ad.rp_id_hash, &sha256(rp_id.as_bytes())) {
        return Err("rpIdHash mismatch");
    }
    if ad.flags & FLAG_UP == 0 {
        return Err("user not present");
    }
    if ad.flags & FLAG_UV == 0 {
        return Err("user not verified");
    }
    Ok(())
}

fn cbor_map_get(m: &[(Cbor, Cbor)], key: i64) -> Option<&Cbor> {
    m.iter()
        .find(|(k, _)| k.as_integer().map(i128::from) == Some(key as i128))
        .map(|(_, v)| v)
}

fn cbor_int(v: Option<&Cbor>) -> Option<i64> {
    v?.as_integer()
        .and_then(|i| i64::try_from(i128::from(i)).ok())
}

enum PublicKey {
    P256(p256::ecdsa::VerifyingKey),
    Ed25519(ed25519_dalek::VerifyingKey),
}

fn parse_cose_key(cose: &[u8]) -> Result<(i64, PublicKey), &'static str> {
    let v: Cbor = ciborium::de::from_reader(cose).map_err(|_| "bad COSE key")?;
    let m = v.as_map().ok_or("COSE key is not a map")?;
    let kty = cbor_int(cbor_map_get(m, 1)).ok_or("COSE kty missing")?;
    let alg = cbor_int(cbor_map_get(m, 3)).ok_or("COSE alg missing")?;
    let crv = cbor_int(cbor_map_get(m, -1)).ok_or("COSE crv missing")?;
    let x = cbor_map_get(m, -2)
        .and_then(Cbor::as_bytes)
        .ok_or("COSE x missing")?;
    match (kty, alg, crv) {
        (2, ALG_ES256, 1) => {
            let y = cbor_map_get(m, -3)
                .and_then(Cbor::as_bytes)
                .ok_or("COSE y missing")?;
            if x.len() != 32 || y.len() != 32 {
                return Err("bad P-256 coordinates");
            }
            let mut sec1 = vec![0x04];
            sec1.extend_from_slice(x);
            sec1.extend_from_slice(y);
            let vk = p256::ecdsa::VerifyingKey::from_sec1_bytes(&sec1)
                .map_err(|_| "P-256 point not on curve")?;
            Ok((alg, PublicKey::P256(vk)))
        }
        (1, ALG_EDDSA, 6) => {
            let x: [u8; 32] = x.as_slice().try_into().map_err(|_| "bad Ed25519 key")?;
            Ok((
                alg,
                PublicKey::Ed25519(
                    ed25519_dalek::VerifyingKey::from_bytes(&x).map_err(|_| "bad Ed25519 key")?,
                ),
            ))
        }
        _ => Err("unsupported credential algorithm (ES256 or EdDSA only)"),
    }
}

pub fn verify_registration(
    client_data_json: &[u8],
    attestation_object: &[u8],
    raw_id: &[u8],
    challenge: &[u8],
    rp_id: &str,
    origins: &[String],
) -> Result<NewCredential, &'static str> {
    check_client_data(client_data_json, "webauthn.create", challenge, origins)?;
    let att: Cbor = ciborium::de::from_reader(attestation_object)
        .map_err(|_| "attestationObject is not CBOR")?;
    let m = att.as_map().ok_or("attestationObject is not a map")?;
    let get = |k: &str| {
        m.iter()
            .find(|(key, _)| key.as_text() == Some(k))
            .map(|(_, v)| v)
    };
    let fmt = get("fmt")
        .and_then(Cbor::as_text)
        .ok_or("attestation fmt missing")?;
    if fmt == "none"
        && get("attStmt")
            .and_then(Cbor::as_map)
            .is_none_or(|s| !s.is_empty())
    {
        return Err("fmt none needs an empty attStmt");
    }
    let auth_data = get("authData")
        .and_then(Cbor::as_bytes)
        .ok_or("authData missing")?;
    let ad = parse_auth_data(auth_data)?;
    check_flags_and_rp(&ad, rp_id)?;
    let (aaguid, cred_id, cose) = ad.attested.ok_or("no attested credential data")?;
    if !ct_eq(&cred_id, raw_id) {
        return Err("credential id does not match rawId");
    }
    let (alg, _) = parse_cose_key(&cose)?;
    Ok(NewCredential {
        cred_id,
        cose_key: cose,
        alg,
        sign_count: ad.sign_count,
        aaguid,
        backup_eligible: ad.flags & FLAG_BE != 0,
    })
}

/// Returns the new signature counter to store.
#[allow(clippy::too_many_arguments)]
pub fn verify_assertion(
    client_data_json: &[u8],
    authenticator_data: &[u8],
    signature: &[u8],
    challenge: &[u8],
    rp_id: &str,
    origins: &[String],
    cose_key: &[u8],
    stored_count: u32,
) -> Result<u32, &'static str> {
    check_client_data(client_data_json, "webauthn.get", challenge, origins)?;
    let ad = parse_auth_data(authenticator_data)?;
    check_flags_and_rp(&ad, rp_id)?;
    let mut signed = authenticator_data.to_vec();
    signed.extend_from_slice(&sha256(client_data_json));
    match parse_cose_key(cose_key)?.1 {
        PublicKey::P256(vk) => {
            use p256::ecdsa::signature::Verifier;
            let sig = p256::ecdsa::Signature::from_der(signature)
                .map_err(|_| "bad ES256 signature encoding")?;
            let sig = sig.normalize_s().unwrap_or(sig);
            vk.verify(&signed, &sig).map_err(|_| "bad signature")?;
        }
        PublicKey::Ed25519(vk) => {
            let sig: [u8; 64] = signature
                .try_into()
                .map_err(|_| "bad EdDSA signature length")?;
            vk.verify_strict(&signed, &ed25519_dalek::Signature::from_bytes(&sig))
                .map_err(|_| "bad signature")?;
        }
    }
    if (ad.sign_count != 0 || stored_count != 0) && ad.sign_count <= stored_count {
        return Err("signature counter did not increase (possible cloned authenticator)");
    }
    Ok(ad.sign_count)
}

/// A software passkey authenticator (P-256, `fmt: none`) used by the tests and by the guide's local walkthrough.
#[doc(hidden)]
pub mod soft {
    use super::*;
    use crate::util::{b64url, random_bytes};
    use p256::ecdsa::{SigningKey, signature::Signer};
    use serde_json::{Value, json};

    pub struct SoftAuthenticator {
        pub key: SigningKey,
        pub cred_id: Vec<u8>,
        pub user_handle: Vec<u8>,
        pub counter: u32,
        pub rp_id: String,
        pub origin: String,
    }

    impl SoftAuthenticator {
        pub fn new(rp_id: &str, origin: &str) -> SoftAuthenticator {
            SoftAuthenticator {
                key: SigningKey::from_slice(&random_bytes::<32>()).expect("random scalar"),
                cred_id: random_bytes::<16>().to_vec(),
                user_handle: Vec::new(),
                counter: 0,
                rp_id: rp_id.into(),
                origin: origin.into(),
            }
        }

        fn auth_data(&self, flags: u8, attested: bool) -> Vec<u8> {
            let mut d = sha256(self.rp_id.as_bytes()).to_vec();
            d.push(flags | if attested { FLAG_AT } else { 0 });
            d.extend_from_slice(&self.counter.to_be_bytes());
            if attested {
                d.extend_from_slice(&[0u8; 16]);
                d.extend_from_slice(&(self.cred_id.len() as u16).to_be_bytes());
                d.extend_from_slice(&self.cred_id);
                let pt = self.key.verifying_key().to_encoded_point(false);
                let cose = Cbor::Map(vec![
                    (Cbor::Integer(1.into()), Cbor::Integer(2.into())),
                    (Cbor::Integer(3.into()), Cbor::Integer((-7).into())),
                    (Cbor::Integer((-1).into()), Cbor::Integer(1.into())),
                    (
                        Cbor::Integer((-2).into()),
                        Cbor::Bytes(pt.x().unwrap().to_vec()),
                    ),
                    (
                        Cbor::Integer((-3).into()),
                        Cbor::Bytes(pt.y().unwrap().to_vec()),
                    ),
                ]);
                ciborium::ser::into_writer(&cose, &mut d).unwrap();
            }
            d
        }

        fn client_data(&self, ty: &str, challenge: &str) -> Vec<u8> {
            json!({"type": ty, "challenge": challenge, "origin": self.origin, "crossOrigin": false})
                .to_string()
                .into_bytes()
        }

        /// Answers `publicKey` creation options with a `PublicKeyCredential` JSON (base64url fields).
        pub fn create(&mut self, options: &Value) -> Value {
            let challenge = options["challenge"].as_str().expect("challenge");
            self.user_handle =
                b64_decode(options["user"]["id"].as_str().expect("user.id")).unwrap();
            let cd = self.client_data("webauthn.create", challenge);
            let att = Cbor::Map(vec![
                (Cbor::Text("fmt".into()), Cbor::Text("none".into())),
                (Cbor::Text("attStmt".into()), Cbor::Map(vec![])),
                (
                    Cbor::Text("authData".into()),
                    Cbor::Bytes(self.auth_data(FLAG_UP | FLAG_UV | FLAG_BE, true)),
                ),
            ]);
            let mut att_bytes = Vec::new();
            ciborium::ser::into_writer(&att, &mut att_bytes).unwrap();
            json!({
                "id": b64url(&self.cred_id), "rawId": b64url(&self.cred_id), "type": "public-key",
                "response": {"clientDataJSON": b64url(&cd), "attestationObject": b64url(&att_bytes), "transports": ["internal"]}
            })
        }

        /// Answers `publicKey` request options with an assertion. `uv` = user verified.
        pub fn get(&mut self, options: &Value, uv: bool) -> Value {
            let challenge = options["challenge"].as_str().expect("challenge");
            self.counter += 1;
            let ad = self.auth_data(FLAG_UP | if uv { FLAG_UV } else { 0 } | FLAG_BE, false);
            let cd = self.client_data("webauthn.get", challenge);
            let mut signed = ad.clone();
            signed.extend_from_slice(&sha256(&cd));
            let sig: p256::ecdsa::Signature = self.key.sign(&signed);
            json!({
                "id": b64url(&self.cred_id), "rawId": b64url(&self.cred_id), "type": "public-key",
                "response": {"clientDataJSON": b64url(&cd), "authenticatorData": b64url(&ad),
                             "signature": b64url(sig.to_der().as_bytes()), "userHandle": b64url(&self.user_handle)}
            })
        }
    }
}

#[cfg(test)]
mod tests {
    use super::soft::SoftAuthenticator;
    use super::*;
    use crate::util::b64url;
    use serde_json::json;

    fn origins() -> Vec<String> {
        vec!["https://pretend.napple.lat".into()]
    }

    fn bytes(v: &serde_json::Value) -> Vec<u8> {
        b64_decode(v.as_str().unwrap()).unwrap()
    }

    #[test]
    fn register_then_assert() {
        let ch = [7u8; 32];
        let opts = json!({"challenge": b64url(&ch), "user": {"id": b64url(&[1u8; 16])}});
        let mut a = SoftAuthenticator::new("napple.lat", "https://pretend.napple.lat");
        let c = a.create(&opts);
        let r = &c["response"];
        let cred = verify_registration(
            &bytes(&r["clientDataJSON"]),
            &bytes(&r["attestationObject"]),
            &bytes(&c["rawId"]),
            &ch,
            "napple.lat",
            &origins(),
        )
        .unwrap();
        assert_eq!(cred.alg, ALG_ES256);
        assert!(cred.backup_eligible);

        let ch2 = [9u8; 32];
        let g = a.get(&json!({"challenge": b64url(&ch2)}), true);
        let r = &g["response"];
        let n = verify_assertion(
            &bytes(&r["clientDataJSON"]),
            &bytes(&r["authenticatorData"]),
            &bytes(&r["signature"]),
            &ch2,
            "napple.lat",
            &origins(),
            &cred.cose_key,
            0,
        )
        .unwrap();
        assert_eq!(n, 1);
        // Replay with the same counter is refused.
        assert!(
            verify_assertion(
                &bytes(&r["clientDataJSON"]),
                &bytes(&r["authenticatorData"]),
                &bytes(&r["signature"]),
                &ch2,
                "napple.lat",
                &origins(),
                &cred.cose_key,
                1
            )
            .is_err()
        );
        // Wrong challenge, origin, rp id.
        let args = |ch: &[u8], rp: &str, o: &[String]| {
            verify_assertion(
                &bytes(&r["clientDataJSON"]),
                &bytes(&r["authenticatorData"]),
                &bytes(&r["signature"]),
                ch,
                rp,
                o,
                &cred.cose_key,
                0,
            )
        };
        assert_eq!(
            args(&ch, "napple.lat", &origins()),
            Err("challenge mismatch")
        );
        assert_eq!(args(&ch2, "evil.lat", &origins()), Err("rpIdHash mismatch"));
        assert_eq!(
            args(&ch2, "napple.lat", &["https://evil.lat".to_string()]),
            Err("origin not allowed")
        );
        // Without user verification.
        let g = a.get(&json!({"challenge": b64url(&ch2)}), false);
        let r = &g["response"];
        assert_eq!(
            verify_assertion(
                &bytes(&r["clientDataJSON"]),
                &bytes(&r["authenticatorData"]),
                &bytes(&r["signature"]),
                &ch2,
                "napple.lat",
                &origins(),
                &cred.cose_key,
                0
            ),
            Err("user not verified")
        );
    }

    #[test]
    fn tampered_signature_and_wrong_type() {
        let ch = [3u8; 32];
        let mut a = SoftAuthenticator::new("napple.lat", "https://pretend.napple.lat");
        let c = a.create(&json!({"challenge": b64url(&ch), "user": {"id": b64url(&[1u8; 16])}}));
        let r = &c["response"];
        let cred = verify_registration(
            &bytes(&r["clientDataJSON"]),
            &bytes(&r["attestationObject"]),
            &bytes(&c["rawId"]),
            &ch,
            "napple.lat",
            &origins(),
        )
        .unwrap();
        // A registration response is not an assertion.
        assert!(
            verify_registration(
                &bytes(&r["clientDataJSON"]),
                &bytes(&r["attestationObject"]),
                &[0u8; 16],
                &ch,
                "napple.lat",
                &origins()
            )
            .is_err()
        );
        let g = a.get(&json!({"challenge": b64url(&ch)}), true);
        let r = &g["response"];
        let mut ad = bytes(&r["authenticatorData"]);
        ad[36] ^= 0x40; // counter byte changes the signed data
        assert_eq!(
            verify_assertion(
                &bytes(&r["clientDataJSON"]),
                &ad,
                &bytes(&r["signature"]),
                &ch,
                "napple.lat",
                &origins(),
                &cred.cose_key,
                0
            ),
            Err("bad signature")
        );
    }
}
