//! Device keys, account save key wrapping (HPKE) and the recovery code.
//!
//! - Device key: X25519, generated at sign-in, private half only in the secret store (Keychain). The Secure Enclave holds P-256 keys only, so
//!   it cannot hold this key; the Keychain item is device-only (not synchronized, after-first-unlock on iOS).
//! - Account save key: 32 random bytes made by the account's first device. Wrapped to a device public key with HPKE base mode
//!   (RFC 9180: DHKEM(X25519, HKDF-SHA256), HKDF-SHA256, AES-256-GCM), info = "napple-account-save-key-v1", aad empty;
//!   wire form = enc (32 bytes) || ciphertext (48 bytes), base64url.
//! - Recovery: 24 BIP-39 English words (256 bits of entropy). KEK = Argon2id(password = the entropy, salt 16 random bytes, m/t/p in the record),
//!   record = {kdf:"argon2id", m_kib, t, p, salt, wrapped = nonce(12) || AES-256-GCM(KEK, account key, aad "napple-recovery-v1")}.
use crate::blob::{aes_gcm_open, aes_gcm_seal};
use crate::util::{b64, random, unb64};
use crate::{Error, Result};
use hpke::aead::AesGcm256;
use hpke::kdf::HkdfSha256;
use hpke::kem::X25519HkdfSha256;
use hpke::{Deserializable, Kem, OpModeR, OpModeS, Serializable};
use rand_core::{OsRng, TryRngCore};
use serde::{Deserialize, Serialize};

pub const WRAP_INFO: &[u8] = b"napple-account-save-key-v1";
type K = X25519HkdfSha256;

pub fn device_keypair() -> ([u8; 32], [u8; 32]) {
    let (sk, pk) = K::gen_keypair(&mut OsRng.unwrap_err());
    (sk.to_bytes().into(), pk.to_bytes().into())
}

pub fn device_public(sk: &[u8]) -> Result<[u8; 32]> {
    let sk = <K as Kem>::PrivateKey::from_bytes(sk).map_err(|e| Error::Crypto(format!("device key: {e:?}")))?;
    Ok(K::sk_to_pk(&sk).to_bytes().into())
}

pub fn wrap_account_key(account_key: &[u8; 32], device_pub: &[u8]) -> Result<Vec<u8>> {
    let pk = <K as Kem>::PublicKey::from_bytes(device_pub).map_err(|e| Error::Crypto(format!("device public key: {e:?}")))?;
    let (enc, ct) = hpke::single_shot_seal::<AesGcm256, HkdfSha256, K, _>(&OpModeS::Base, &pk, WRAP_INFO, account_key, b"", &mut OsRng.unwrap_err())
        .map_err(|e| Error::Crypto(format!("HPKE seal: {e:?}")))?;
    let mut out = enc.to_bytes().to_vec();
    out.extend_from_slice(&ct);
    Ok(out)
}

pub fn unwrap_account_key(device_sk: &[u8], wrapped: &[u8]) -> Result<[u8; 32]> {
    if wrapped.len() != 32 + 32 + 16 {
        return Err(Error::Crypto(format!("wrapped account key has {} bytes, expected 80", wrapped.len())));
    }
    let sk = <K as Kem>::PrivateKey::from_bytes(device_sk).map_err(|e| Error::Crypto(format!("device key: {e:?}")))?;
    let enc = <K as Kem>::EncappedKey::from_bytes(&wrapped[..32]).map_err(|e| Error::Crypto(format!("HPKE enc: {e:?}")))?;
    let pt = hpke::single_shot_open::<AesGcm256, HkdfSha256, K>(&OpModeR::Base, &sk, &enc, WRAP_INFO, &wrapped[32..], b"")
        .map_err(|_| Error::Crypto("the wrapped account key does not open with this device's key".into()))?;
    pt.try_into().map_err(|_| Error::Crypto("account key is not 32 bytes".into()))
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct RecoveryRecord {
    pub kdf: String,
    pub m_kib: u32,
    pub t: u32,
    pub p: u32,
    pub salt: String,
    pub wrapped: String,
}

/// Argon2id cost for new recovery records (64 MiB, 3 passes: about half a second on a phone).
pub const RECOVERY_COST: (u32, u32, u32) = (64 * 1024, 3, 1);

/// A new recovery phrase (24 words) and its entropy.
pub fn recovery_phrase() -> (String, [u8; 32]) {
    let e: [u8; 32] = random();
    let m = bip39::Mnemonic::from_entropy(&e).expect("32 bytes is valid BIP-39 entropy");
    (m.to_string(), e)
}

/// The entropy of a typed phrase (case, extra spaces and line breaks ignored; the BIP-39 checksum catches typos).
pub fn recovery_entropy(phrase: &str) -> Result<[u8; 32]> {
    let norm = phrase.split_whitespace().map(|w| w.to_lowercase()).collect::<Vec<_>>().join(" ");
    let m = bip39::Mnemonic::parse_normalized(&norm).map_err(|e| Error::Crypto(format!("recovery code: {e}")))?;
    m.to_entropy().try_into().map_err(|_| Error::Crypto("the recovery code must have 24 words".into()))
}

fn kek(entropy: &[u8; 32], salt: &[u8], m: u32, t: u32, p: u32) -> Result<[u8; 32]> {
    let params = argon2::Params::new(m, t, p, Some(32)).map_err(|e| Error::Crypto(format!("argon2 params: {e}")))?;
    let a = argon2::Argon2::new(argon2::Algorithm::Argon2id, argon2::Version::V0x13, params);
    let mut out = [0u8; 32];
    a.hash_password_into(entropy, salt, &mut out).map_err(|e| Error::Crypto(format!("argon2: {e}")))?;
    Ok(out)
}

pub fn recovery_wrap(account_key: &[u8; 32], entropy: &[u8; 32], cost: (u32, u32, u32)) -> Result<RecoveryRecord> {
    let salt: [u8; 16] = random();
    let k = kek(entropy, &salt, cost.0, cost.1, cost.2)?;
    let nonce: [u8; 12] = random();
    let mut w = nonce.to_vec();
    w.extend_from_slice(&aes_gcm_seal(&k, &nonce, account_key, b"napple-recovery-v1")?);
    Ok(RecoveryRecord { kdf: "argon2id".into(), m_kib: cost.0, t: cost.1, p: cost.2, salt: b64(&salt), wrapped: b64(&w) })
}

pub fn recovery_unwrap(r: &RecoveryRecord, entropy: &[u8; 32]) -> Result<[u8; 32]> {
    if r.kdf != "argon2id" || r.m_kib > 1 << 21 || r.t > 64 || r.p > 16 {
        return Err(Error::Crypto("unsupported recovery record".into()));
    }
    let salt = unb64(&r.salt).ok_or_else(|| Error::Format("recovery salt".into()))?;
    let w = unb64(&r.wrapped).ok_or_else(|| Error::Format("recovery record".into()))?;
    if w.len() != 12 + 32 + 16 {
        return Err(Error::Format("recovery record size".into()));
    }
    let k = kek(entropy, &salt, r.m_kib, r.t, r.p)?;
    let nonce: [u8; 12] = w[..12].try_into().unwrap();
    let pt = aes_gcm_open(&k, &nonce, &w[12..], b"napple-recovery-v1").map_err(|_| Error::Crypto("wrong recovery code".into()))?;
    pt.try_into().map_err(|_| Error::Crypto("account key is not 32 bytes".into()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::util::{hex, unhex};

    /// RFC 9180 test vector, mode 0 (base), KEM 0x0020 DHKEM(X25519, HKDF-SHA256), KDF 0x0001 HKDF-SHA256, AEAD 0x0002 AES-256-GCM,
    /// sequence 0 (cfrg/draft-irtf-cfrg-hpke test-vectors.json): the receiver side this client uses to unwrap the account key.
    #[test]
    fn hpke_rfc9180_vector_unwrap() {
        let sk_rm = unhex("497b4502664cfea5d5af0b39934dac72242a74f8480451e1aee7d6a53320333d").unwrap();
        let pk_rm = unhex("430f4b9859665145a6b1ba274024487bd66f03a2dd577d7753c68d7d7d00c00c").unwrap();
        let enc = unhex("6c93e09869df3402d7bf231bf540fadd35cd56be14f97178f0954db94b7fc256").unwrap();
        let info = unhex("4f6465206f6e2061204772656369616e2055726e").unwrap();
        let aad = unhex("436f756e742d30").unwrap();
        let ct = unhex("e5d84cd531cfb583096e7cfa9641bd3079cf3a91cda813c52deb5f512be9931980a41de125a925cdad859d5b7a").unwrap();
        assert_eq!(hex(&device_public(&sk_rm).unwrap()), hex(&pk_rm));
        let sk = <K as Kem>::PrivateKey::from_bytes(&sk_rm).unwrap();
        let encapped = <K as Kem>::EncappedKey::from_bytes(&enc).unwrap();
        let mut ctx = hpke::setup_receiver::<AesGcm256, HkdfSha256, K>(&OpModeR::Base, &sk, &encapped, &info).unwrap();
        let pt = ctx.open(&ct, &aad).unwrap();
        assert_eq!(hex(&pt), "4265617574792069732074727574682c20747275746820626561757479");
    }

    #[test]
    fn wrap_unwrap() {
        let (sk_a, pk_a) = device_keypair();
        let (sk_b, _) = device_keypair();
        let key: [u8; 32] = random();
        let w = wrap_account_key(&key, &pk_a).unwrap();
        assert_eq!(w.len(), 80);
        assert_eq!(unwrap_account_key(&sk_a, &w).unwrap(), key);
        assert!(unwrap_account_key(&sk_b, &w).is_err());
        let mut bad = w.clone();
        bad[50] ^= 1;
        assert!(unwrap_account_key(&sk_a, &bad).is_err());
        assert_ne!(wrap_account_key(&key, &pk_a).unwrap(), w, "fresh ephemeral key per wrap");
    }

    #[test]
    fn recovery_round_trip() {
        let key: [u8; 32] = random();
        let (phrase, e) = recovery_phrase();
        assert_eq!(phrase.split(' ').count(), 24);
        let r = recovery_wrap(&key, &e, (256, 1, 1)).unwrap();
        let typed = format!("  {}\n", phrase.to_uppercase().replace(' ', "  "));
        assert_eq!(recovery_unwrap(&r, &recovery_entropy(&typed).unwrap()).unwrap(), key);
        let (_, other) = recovery_phrase();
        assert!(recovery_unwrap(&r, &other).is_err());
        let mut words: Vec<&str> = phrase.split(' ').collect();
        words.swap(0, 1);
        if words[0] != words[1] {
            assert!(recovery_entropy(&words.join(" ")).is_err() || recovery_unwrap(&r, &recovery_entropy(&words.join(" ")).unwrap()).is_err());
        }
    }
}
