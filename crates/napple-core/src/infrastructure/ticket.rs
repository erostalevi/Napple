use crate::domain::error::{Error, Result, container};
use crate::domain::keys::KeySet;
use num_bigint::BigUint;
use sha2::{Digest, Sha256};

pub struct Ticket {
    pub rights_id: [u8; 16],
    pub encrypted_title_key: [u8; 16],
    /// The 0x100-byte title key block: the key itself (common) or its RSA-OAEP ciphertext (personalized).
    pub title_key_block: [u8; 0x100],
    pub key_type: u8, // 0 common, 1 personalized
    pub master_key_rev: u8,
}

pub fn parse_ticket(d: &[u8]) -> Result<Ticket> {
    if d.len() < 4 {
        return container("ticket too small");
    }
    let sig_pad = match u32::from_le_bytes(d[..4].try_into().unwrap()) {
        0x10000 | 0x10003 => 0x23C,
        0x10001 | 0x10004 => 0x13C,
        0x10002 | 0x10005 => 0x7C,
        _ => return container("unknown ticket signature type"),
    };
    let b = 4 + sig_pad;
    if d.len() < b + 0x170 {
        return container("ticket truncated");
    }
    Ok(Ticket {
        rights_id: d[b + 0x160..b + 0x170].try_into().unwrap(),
        encrypted_title_key: d[b + 0x40..b + 0x50].try_into().unwrap(),
        title_key_block: d[b + 0x40..b + 0x140].try_into().unwrap(),
        key_type: d[b + 0x141],
        master_key_rev: d[b + 0x145],
    })
}

/// Name of the user-supplied console key (hex of the 0x230-byte decrypted eticket device key:
/// private exponent D at 0x00, modulus N at 0x100, public exponent E at 0x200, big-endian).
pub const DEVICE_KEY: &str = "eticket_rsa_keypair";

fn mgf1(seed: &[u8], len: usize) -> Vec<u8> {
    let mut out = Vec::with_capacity(len + 32);
    let mut c = 0u32;
    while out.len() < len {
        let mut h = Sha256::new();
        h.update(seed);
        h.update(c.to_be_bytes());
        out.extend_from_slice(&h.finalize());
        c += 1;
    }
    out.truncate(len);
    out
}

/// RSA-OAEP (SHA-256, MGF1-SHA-256, empty label) decryption with a raw (d, n) key. Every failure
/// reports the same error so it cannot act as a padding oracle.
pub fn oaep_sha256_decrypt(ct: &[u8], d: &[u8], n: &[u8]) -> Result<Vec<u8>> {
    let bad = || Error::Integrity("personalized ticket title key".into());
    let k = n.len();
    let nn = BigUint::from_bytes_be(n);
    if k < 2 * 32 + 2 || ct.len() != k || nn.bits() != (k * 8) as u64 {
        return Err(bad());
    }
    let c = BigUint::from_bytes_be(ct);
    if c >= nn {
        return Err(bad());
    }
    let m = c.modpow(&BigUint::from_bytes_be(d), &nn).to_bytes_be();
    let mut em = vec![0u8; k - m.len()];
    em.extend_from_slice(&m);
    let (y, rest) = em.split_at_mut(1);
    let (seed, db) = rest.split_at_mut(32);
    for (s, m) in seed.iter_mut().zip(mgf1(db, 32)) {
        *s ^= m;
    }
    for (b, m) in db.iter_mut().zip(mgf1(seed, k - 33)) {
        *b ^= m;
    }
    let lhash = Sha256::digest(b"");
    let mut ok = y[0] == 0 && db[..32] == lhash[..];
    let mut i = 32;
    while i < db.len() && db[i] == 0 {
        i += 1;
    }
    ok &= i < db.len() && db[i] == 1;
    if !ok {
        return Err(bad());
    }
    Ok(db[i + 1..].to_vec())
}

/// The title key as stored in an NCA key area-style form (still encrypted by the titlekek), for
/// the `TitleKeys` map. Common tickets carry it directly; personalized ones are RSA-OAEP
/// encrypted to the console's eticket device key, which the user supplies in prod.keys as
/// `eticket_rsa_keypair`. `Ok(None)` when the ticket is personalized and that key is absent.
pub fn encrypted_title_key(t: &Ticket, keys: &KeySet) -> Result<Option<[u8; 16]>> {
    match t.key_type {
        0 => Ok(Some(t.encrypted_title_key)),
        1 => {
            let Some(kp) = keys.get_vec(DEVICE_KEY) else {
                return Ok(None);
            };
            if kp.len() < 0x204 {
                return Err(Error::KeyFile(format!("key {DEVICE_KEY} has wrong length")));
            }
            let pt = oaep_sha256_decrypt(&t.title_key_block, &kp[..0x100], &kp[0x100..0x200])?;
            let k: [u8; 16] = pt
                .as_slice()
                .try_into()
                .map_err(|_| Error::Integrity("personalized ticket title key length".into()))?;
            Ok(Some(k))
        }
        _ => Ok(None),
    }
}

#[cfg(test)]
mod ticket_testkey;

#[cfg(test)]
mod tests {
    use super::ticket_testkey::{CT, D, N};
    use super::*;

    fn unhex(s: &str) -> Vec<u8> {
        (0..s.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
            .collect()
    }

    fn ticket(key_type: u8, block: &[u8]) -> Ticket {
        Ticket {
            rights_id: [1; 16],
            encrypted_title_key: block[..16].try_into().unwrap(),
            title_key_block: block.try_into().unwrap(),
            key_type,
            master_key_rev: 0,
        }
    }

    fn keys_with_device() -> KeySet {
        let mut kp = unhex(D);
        kp.extend(unhex(N));
        kp.extend([0, 1, 0, 1]);
        kp.resize(0x230, 0);
        let hex: String = kp.iter().map(|b| format!("{b:02x}")).collect();
        KeySet::parse(&format!("{DEVICE_KEY} = {hex}\n")).unwrap()
    }

    #[test]
    fn oaep_matches_openssl_vector() {
        let pt = oaep_sha256_decrypt(&unhex(CT), &unhex(D), &unhex(N)).unwrap();
        assert_eq!(pt, b"0123456789abcdef");
    }

    #[test]
    fn oaep_rejects_corruption() {
        let mut ct = unhex(CT);
        ct[100] ^= 1;
        assert!(oaep_sha256_decrypt(&ct, &unhex(D), &unhex(N)).is_err());
        assert!(oaep_sha256_decrypt(&ct[1..], &unhex(D), &unhex(N)).is_err());
        let mut n = unhex(N);
        n[0] = 0; // modulus no longer the claimed size
        assert!(oaep_sha256_decrypt(&unhex(CT), &unhex(D), &n).is_err());
    }

    #[test]
    fn personalized_ticket_resolves_with_device_key() {
        let t = ticket(1, &unhex(CT));
        let k = encrypted_title_key(&t, &keys_with_device())
            .unwrap()
            .unwrap();
        assert_eq!(&k, b"0123456789abcdef");
    }

    #[test]
    fn personalized_without_device_key_is_skipped() {
        let t = ticket(1, &unhex(CT));
        let ks = KeySet::parse("header_key = 00112233445566778899aabbccddeeff\n").unwrap();
        assert_eq!(encrypted_title_key(&t, &ks).unwrap(), None);
    }

    #[test]
    fn common_ticket_passes_through() {
        let mut blk = [0u8; 0x100];
        blk[..16].copy_from_slice(b"fedcba9876543210");
        let ks = KeySet::parse("header_key = 00112233445566778899aabbccddeeff\n").unwrap();
        let k = encrypted_title_key(&ticket(0, &blk), &ks).unwrap().unwrap();
        assert_eq!(&k, b"fedcba9876543210");
    }

    #[test]
    fn wrong_device_key_is_integrity_error() {
        let mut t = ticket(1, &unhex(CT));
        t.title_key_block[5] ^= 0x40;
        assert!(matches!(
            encrypted_title_key(&t, &keys_with_device()),
            Err(Error::Integrity(_))
        ));
    }
}
