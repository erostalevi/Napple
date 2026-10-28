//! Shared helpers for the fuzz targets: the throwaway synthetic key set (same one the tests use) and
//! the NCA header trick. NCA headers are AES-XTS encrypted, so a byte-level fuzzer would never get past
//! the magic check; the NCA targets therefore treat the input as the DECRYPTED header (first 0xC00 bytes)
//! plus body and encrypt the header here before parsing.
use aes::Aes128;
use aes::cipher::{KeyInit, generic_array::GenericArray};
use napple_core::domain::keys::KeySet;
use napple_core::infrastructure::io::{BytesSource, Src};
use std::sync::Arc;
use xts_mode::Xts128;

pub const KEYS: &str =
    include_str!("../../crates/napple-core/tests/fixtures/nsz/synthetic_keys.txt");

pub fn keys() -> KeySet {
    KeySet::parse(KEYS).unwrap()
}

pub fn src(d: &[u8]) -> Src {
    Arc::new(BytesSource(d.to_vec()))
}

/// Encrypts the header of `plain` (decrypted NCA header + body) the way the readers expect.
pub fn encrypt_nca_header(plain: &[u8]) -> Vec<u8> {
    let mut v = plain.to_vec();
    if v.len() < 0xC00 {
        return v;
    }
    let hk = keys().get::<32>("header_key").unwrap();
    let xts = Xts128::<Aes128>::new(
        Aes128::new(GenericArray::from_slice(&hk[..16])),
        Aes128::new(GenericArray::from_slice(&hk[16..])),
    );
    let ver3 = &v[0x200..0x204] == b"NCA3";
    for i in 0..6 {
        let n = if ver3 || i < 2 { i as u128 } else { 0 };
        xts.encrypt_sector(&mut v[i * 0x200..(i + 1) * 0x200], n.to_be_bytes());
    }
    v
}

/// Decrypts it back (used by the corpus generator).
pub fn decrypt_nca_header(enc: &[u8]) -> Vec<u8> {
    let mut v = enc.to_vec();
    if v.len() < 0xC00 {
        return v;
    }
    let hk = keys().get::<32>("header_key").unwrap();
    let xts = Xts128::<Aes128>::new(
        Aes128::new(GenericArray::from_slice(&hk[..16])),
        Aes128::new(GenericArray::from_slice(&hk[16..])),
    );
    let mut probe = v[..0x400].to_vec();
    for i in 0..2 {
        xts.decrypt_sector(
            &mut probe[i * 0x200..(i + 1) * 0x200],
            (i as u128).to_be_bytes(),
        );
    }
    let ver3 = &probe[0x200..0x204] == b"NCA3";
    for i in 0..6 {
        let n = if ver3 || i < 2 { i as u128 } else { 0 };
        xts.decrypt_sector(&mut v[i * 0x200..(i + 1) * 0x200], n.to_be_bytes());
    }
    v
}

/// Reads up to `n` bytes of a source (for touching section data).
pub fn touch(s: &Src) {
    let n = s.size().min(1 << 16) as usize;
    let _ = napple_core::infrastructure::io::read_vec(&**s, 0, n);
    if s.size() > 64 {
        let mut b = [0u8; 64];
        let _ = s.read_at(s.size() - 64, &mut b);
    }
}

/// Opens a decrypted-header NCA and walks everything reachable from it.
pub fn walk_nca(plain: &[u8]) {
    use napple_core::infrastructure::nca::{self, FsType, Nca, TitleKeys};
    use napple_core::infrastructure::{pfs::parse_pfs0, romfs::RomFs};
    let ks = keys();
    let Ok(n) = Nca::open(src(&encrypt_nca_header(plain)), &ks, &TitleKeys::new()) else {
        return;
    };
    for sec in &n.sections {
        let Ok(s) = n.section(sec.index) else {
            continue;
        };
        touch(&s);
        match sec.fs_type {
            FsType::Pfs0 => {
                if let Ok(Ok(fs)) = nca::pfs0_data(&s, sec).map(parse_pfs0) {
                    for e in &fs.entries {
                        if let Ok(f) = fs.open(&e.name) {
                            touch(&f);
                        }
                    }
                }
            }
            FsType::RomFs => {
                if let Ok(d) = nca::romfs_data(&s, sec) {
                    if let Ok(rf) = RomFs::parse(d) {
                        for f in rf.files.iter().take(32) {
                            if let Ok(x) = rf.open(f) {
                                touch(&x);
                            }
                        }
                    }
                }
                let _ = nca::romfs_data_verified(&s, sec);
            }
        }
    }
}
