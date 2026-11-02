//! Encrypted save blob, format 1 (what the server stores; it never sees plaintext or keys).
//!
//! ```text
//! 0   4   magic "NSAV"
//! 4   1   format = 1
//! 5   1   aead   = 1 (AES-256-GCM)
//! 6   1   comp   = 1 (zstd)
//! 7   1   reserved = 0
//! 8   12  nonce
//! 20  ..  AES-256-GCM(key, nonce, zstd(tar), aad = bytes 0..20) || 16-byte tag
//! ```
//! key   = HKDF-SHA256(ikm = account save key, salt = none, info = "napple-save|<TITLE>|<profile_id>|<version>"), 32 bytes
//! nonce = HMAC-SHA256(key, "napple-save-nonce" || zstd(tar))[0..12]  (synthetic: deterministic, and a different plaintext under the same
//!         per-version key, e.g. a retried upload of a newer save, never reuses a nonce)
//! The same save at the same version therefore always encrypts to the same bytes (same sha256_ciphertext).
use crate::{Error, Result};
use aes_gcm::aead::{Aead, KeyInit, Payload};
use aes_gcm::{Aes256Gcm, Nonce};
use hkdf::Hkdf;
use hmac::{Hmac, Mac};
use sha2::Sha256;

pub const MAGIC: &[u8; 4] = b"NSAV";
pub const FORMAT: u8 = 1;
pub const AEAD_AES256GCM: u8 = 1;
pub const COMP_ZSTD: u8 = 1;
pub const HEADER_LEN: usize = 20;
/// Contract: max 64 MiB per version.
pub const MAX_BLOB: usize = 64 << 20;
const ZSTD_LEVEL: i32 = 9;

pub fn version_info(title: &str, profile: &str, version: u64) -> String {
    format!("napple-save|{title}|{profile}|{version}")
}

pub fn version_key(account_key: &[u8; 32], title: &str, profile: &str, version: u64) -> [u8; 32] {
    let hk = Hkdf::<Sha256>::new(None, account_key);
    let mut k = [0u8; 32];
    hk.expand(version_info(title, profile, version).as_bytes(), &mut k).expect("32 bytes is a valid HKDF-SHA256 length");
    k
}

fn synthetic_nonce(key: &[u8; 32], compressed: &[u8]) -> [u8; 12] {
    let mut m = <Hmac<Sha256> as Mac>::new_from_slice(key).expect("HMAC takes any key length");
    m.update(b"napple-save-nonce");
    m.update(compressed);
    let tag = m.finalize().into_bytes();
    let mut n = [0u8; 12];
    n.copy_from_slice(&tag[..12]);
    n
}

/// AES-256-GCM with the header as AAD (exposed for the test vectors).
pub fn aes_gcm_seal(key: &[u8; 32], nonce: &[u8; 12], pt: &[u8], aad: &[u8]) -> Result<Vec<u8>> {
    Aes256Gcm::new(key.into())
        .encrypt(Nonce::from_slice(nonce), Payload { msg: pt, aad })
        .map_err(|_| Error::Crypto("AES-GCM encryption failed".into()))
}

pub fn aes_gcm_open(key: &[u8; 32], nonce: &[u8; 12], ct: &[u8], aad: &[u8]) -> Result<Vec<u8>> {
    Aes256Gcm::new(key.into())
        .decrypt(Nonce::from_slice(nonce), Payload { msg: ct, aad })
        .map_err(|_| Error::Crypto("save blob authentication failed (wrong key, title, profile or version, or corrupted data)".into()))
}

/// tar bytes -> blob for (title, profile, version).
pub fn seal(account_key: &[u8; 32], title: &str, profile: &str, version: u64, tar: &[u8]) -> Result<Vec<u8>> {
    let comp = zstd::bulk::compress(tar, ZSTD_LEVEL).map_err(|e| Error::Format(format!("zstd: {e}")))?;
    let key = version_key(account_key, title, profile, version);
    let nonce = synthetic_nonce(&key, &comp);
    let mut out = Vec::with_capacity(HEADER_LEN + comp.len() + 16);
    out.extend_from_slice(MAGIC);
    out.extend_from_slice(&[FORMAT, AEAD_AES256GCM, COMP_ZSTD, 0]);
    out.extend_from_slice(&nonce);
    let ct = aes_gcm_seal(&key, &nonce, &comp, &out[..HEADER_LEN])?;
    out.extend_from_slice(&ct);
    if out.len() > MAX_BLOB {
        return Err(Error::Format(format!("save is {} bytes encrypted; the limit is 64 MiB per version", out.len())));
    }
    Ok(out)
}

/// blob -> tar bytes (checks the header, authenticates, decompresses with a size cap).
pub fn open(account_key: &[u8; 32], title: &str, profile: &str, version: u64, blob: &[u8]) -> Result<Vec<u8>> {
    if blob.len() < HEADER_LEN + 16 || &blob[..4] != MAGIC {
        return Err(Error::Format("not a Napple save blob".into()));
    }
    if blob[4] != FORMAT || blob[5] != AEAD_AES256GCM || blob[6] != COMP_ZSTD {
        return Err(Error::Format(format!("unsupported save blob format {}/{}/{} (update Napple)", blob[4], blob[5], blob[6])));
    }
    let key = version_key(account_key, title, profile, version);
    let mut nonce = [0u8; 12];
    nonce.copy_from_slice(&blob[8..20]);
    let comp = aes_gcm_open(&key, &nonce, &blob[HEADER_LEN..], &blob[..HEADER_LEN])?;
    zstd::bulk::decompress(&comp, crate::tarball::MAX_TOTAL as usize + (1 << 20)).map_err(|e| Error::Format(format!("zstd: {e}")))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::util::unhex;

    #[test]
    fn hkdf_rfc5869_case1() {
        let ikm = [0x0bu8; 22];
        let salt = unhex("000102030405060708090a0b0c").unwrap();
        let info = unhex("f0f1f2f3f4f5f6f7f8f9").unwrap();
        let hk = Hkdf::<Sha256>::new(Some(&salt), &ikm);
        let mut okm = [0u8; 42];
        hk.expand(&info, &mut okm).unwrap();
        assert_eq!(crate::util::hex(&okm), "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");
    }

    /// AES-256-GCM vector: the key/base_nonce/first encryption of RFC 9180's test vector for mode 0, DHKEM(X25519), HKDF-SHA256, AES-256-GCM
    /// (cfrg/draft-irtf-cfrg-hpke test-vectors.json).
    #[test]
    fn aes256gcm_vector() {
        let key: [u8; 32] = unhex("f50b0609186798729ed0564b36ef2ef8044f1f9d05636874d1f46c819c7a669f").unwrap().try_into().unwrap();
        let nonce: [u8; 12] = unhex("151d9929e2449747889bc923").unwrap().try_into().unwrap();
        let aad = unhex("436f756e742d30").unwrap();
        let pt = unhex("4265617574792069732074727574682c20747275746820626561757479").unwrap();
        let ct = aes_gcm_seal(&key, &nonce, &pt, &aad).unwrap();
        assert_eq!(
            crate::util::hex(&ct),
            "e5d84cd531cfb583096e7cfa9641bd3079cf3a91cda813c52deb5f512be9931980a41de125a925cdad859d5b7a"
        );
        assert_eq!(aes_gcm_open(&key, &nonce, &ct, &aad).unwrap(), pt);
    }

    #[test]
    fn blob_round_trip_and_binding() {
        let k = [42u8; 32];
        let tar = crate::tarball::build(vec![crate::tarball::Entry { path: "account/x".into(), data: Some(vec![9; 5000]) }]).unwrap();
        let b1 = seal(&k, "01006BB00C6F0000", "p1", 3, &tar).unwrap();
        let b2 = seal(&k, "01006BB00C6F0000", "p1", 3, &tar).unwrap();
        assert_eq!(b1, b2, "deterministic");
        assert_eq!(&b1[..8], b"NSAV\x01\x01\x01\x00");
        assert_eq!(open(&k, "01006BB00C6F0000", "p1", 3, &b1).unwrap(), tar);
        // bound to title, profile, version and key
        assert!(open(&k, "0100000000010000", "p1", 3, &b1).is_err());
        assert!(open(&k, "01006BB00C6F0000", "p2", 3, &b1).is_err());
        assert!(open(&k, "01006BB00C6F0000", "p1", 4, &b1).is_err());
        assert!(open(&[1u8; 32], "01006BB00C6F0000", "p1", 3, &b1).is_err());
        // header is authenticated
        let mut bad = b1.clone();
        bad[7] = 1;
        assert!(open(&k, "01006BB00C6F0000", "p1", 3, &bad).is_err());
        let mut bad = b1.clone();
        *bad.last_mut().unwrap() ^= 1;
        assert!(open(&k, "01006BB00C6F0000", "p1", 3, &bad).is_err());
        // a different save under the same version key gets a different nonce
        let tar2 = crate::tarball::build(vec![crate::tarball::Entry { path: "account/x".into(), data: Some(vec![8; 5000]) }]).unwrap();
        let b3 = seal(&k, "01006BB00C6F0000", "p1", 3, &tar2).unwrap();
        assert_ne!(b1[8..20], b3[8..20]);
    }
}
