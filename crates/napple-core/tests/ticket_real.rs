//! The user's own console key from the Testing Material prod.keys: `eticket_rsa_keypair` has the layout
//! `ticket.rs` reads (D at 0x00, N at 0x100, E at 0x200; real dumps are 0x210 bytes), and a personalized
//! ticket built by encrypting a title key to that public key resolves back through `encrypted_title_key`.
//! Skipped unless NAPPLE_TESTING_MATERIAL is set. No key bytes are printed.
use napple_core::domain::keys::KeySet;
use napple_core::infrastructure::ticket::{DEVICE_KEY, Ticket, encrypted_title_key, parse_ticket};
use num_bigint::BigUint;
use sha2::{Digest, Sha256};
use std::path::PathBuf;

fn mgf1(seed: &[u8], len: usize) -> Vec<u8> {
    let mut out = Vec::new();
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

/// RSA-OAEP (SHA-256, MGF1-SHA-256, empty label) encryption with a fixed seed.
fn oaep_encrypt(msg: &[u8], n: &[u8], e: &[u8]) -> Vec<u8> {
    let k = n.len();
    let mut db = Sha256::digest(b"").to_vec();
    db.resize(k - 33 - 1 - msg.len(), 0);
    db.push(1);
    db.extend_from_slice(msg);
    assert_eq!(db.len(), k - 33);
    let seed: Vec<u8> = (0..32u8).map(|i| i.wrapping_mul(7).wrapping_add(3)).collect();
    let mut masked_db = db.clone();
    for (b, m) in masked_db.iter_mut().zip(mgf1(&seed, k - 33)) {
        *b ^= m;
    }
    let mut masked_seed = seed.clone();
    for (b, m) in masked_seed.iter_mut().zip(mgf1(&masked_db, 32)) {
        *b ^= m;
    }
    let mut em = vec![0u8];
    em.extend(masked_seed);
    em.extend(masked_db);
    let c = BigUint::from_bytes_be(&em).modpow(&BigUint::from_bytes_be(e), &BigUint::from_bytes_be(n));
    let mut out = c.to_bytes_be();
    while out.len() < k {
        out.insert(0, 0);
    }
    out
}

#[test]
fn real_console_key_decrypts_a_personalized_ticket() {
    let Some(root) = std::env::var_os("NAPPLE_TESTING_MATERIAL").map(PathBuf::from) else {
        eprintln!("NAPPLE_TESTING_MATERIAL not set: skipped");
        return;
    };
    let keys = KeySet::load(&root.join("Keys 22.1.0/prod.keys")).unwrap();
    let Some(kp) = keys.get_vec(DEVICE_KEY) else {
        eprintln!("prod.keys has no {DEVICE_KEY}: skipped");
        return;
    };
    assert!(kp.len() >= 0x204, "keypair is at least D + N + E");
    let (d, n, e) = (&kp[..0x100], &kp[0x100..0x200], &kp[0x200..0x204]);
    assert_eq!(e, [0, 1, 0, 1], "public exponent 65537 at 0x200");
    assert_eq!(n[0] & 0x80, 0x80, "a 2048-bit modulus");
    // D really is the private exponent of N: m^(e*d) == m
    let nn = BigUint::from_bytes_be(n);
    let m = BigUint::from(0x1234_5678_9abc_def0u64);
    let c = m.modpow(&BigUint::from_bytes_be(e), &nn);
    assert_eq!(c.modpow(&BigUint::from_bytes_be(d), &nn), m, "D inverts E modulo N");

    let title_key = *b"\x00\x11\x22\x33\x44\x55\x66\x77\x88\x99\xaa\xbb\xcc\xdd\xee\xff";
    let block = oaep_encrypt(&title_key, n, e);
    // a v1 (RSA-2048 + SHA-1) ticket body around the block
    let mut tik = Vec::new();
    tik.extend_from_slice(&0x10001u32.to_le_bytes());
    tik.resize(4 + 0x13C, 0);
    let body = tik.len();
    tik.resize(body + 0x2C0, 0);
    tik[body + 0x40..body + 0x140].copy_from_slice(&block);
    tik[body + 0x141] = 1; // personalized
    tik[body + 0x145] = 0x0b;
    tik[body + 0x160..body + 0x170].copy_from_slice(&[0xab; 16]);
    let t: Ticket = parse_ticket(&tik).unwrap();
    assert_eq!(t.key_type, 1);
    assert_eq!(t.rights_id, [0xab; 16]);
    assert_eq!(
        encrypted_title_key(&t, &keys).unwrap(),
        Some(title_key),
        "the real device key resolves the personalized ticket"
    );
    // the same ticket without the console key is skipped, never guessed
    let without = KeySet::parse("header_key = 00112233445566778899aabbccddeeff\n").unwrap();
    assert_eq!(encrypted_title_key(&t, &without).unwrap(), None);
}
