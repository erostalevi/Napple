//! NCA3 (and NCA2/NCA0) reader: AES-XTS header, key-area / titlekey derivation, AES-CTR section decryption.
use super::io::{Slice, Source, Src, check, read_vec};
use super::ivfc::{self, Ivfc};
use super::ticket::oaep_sha256_decrypt;
use crate::domain::error::{Error, Result};
use crate::domain::keys::KeySet;
use aes::Aes128;
use aes::cipher::{
    BlockDecrypt, KeyInit, KeyIvInit, StreamCipher, StreamCipherSeek, generic_array::GenericArray,
};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::sync::Arc;
use xts_mode::Xts128;

const HEADER_SIZE: usize = 0xC00;
const SECTOR: usize = 0x200;
const MEDIA: u64 = 0x200;
/// User-supplied key for NCA0 key areas: hex of RSA private exponent (0x100) || modulus (0x100).
pub const NCA0_KEY: &str = "nca0_key_area_rsa";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ContentType {
    Program,
    Meta,
    Control,
    Manual,
    Data,
    PublicData,
    Other(u8),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FsType {
    RomFs,
    Pfs0,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Enc {
    None,
    Xts,
    Ctr,
    Bktr,
}

#[derive(Debug, Clone)]
pub struct Section {
    pub index: usize,
    pub start: u64,
    pub end: u64,
    pub fs_type: FsType,
    pub hash_type: u8,
    pub enc: Enc,
    pub hash_data: Vec<u8>,
    ctr_hi: [u8; 8],
}

pub struct Nca {
    src: Src,
    pub content_type: ContentType,
    pub program_id: u64,
    pub content_size: u64,
    pub rights_id: [u8; 16],
    pub sections: Vec<Section>,
    key_gen: u8,
    ctr_key: Option<[u8; 16]>,
}

fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn u64le(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}

fn aes_ecb_decrypt(key: &[u8; 16], data: &mut [u8]) {
    let c = Aes128::new(GenericArray::from_slice(key));
    for blk in data.as_chunks_mut::<16>().0 {
        c.decrypt_block(GenericArray::from_mut_slice(blk));
    }
}

pub type TitleKeys = HashMap<[u8; 16], [u8; 16]>; // rights_id -> encrypted title key

impl Nca {
    pub fn open(src: Src, keys: &KeySet, title_keys: &TitleKeys) -> Result<Self> {
        if src.size() < HEADER_SIZE as u64 {
            return Err(Error::Container("NCA too small".into()));
        }
        let mut h = read_vec(&*src, 0, HEADER_SIZE)?;
        let hk: [u8; 32] = keys.get("header_key")?;
        let xts = Xts128::<Aes128>::new(
            Aes128::new(GenericArray::from_slice(&hk[..16])),
            Aes128::new(GenericArray::from_slice(&hk[16..])),
        );
        let decrypt =
            |h: &mut [u8], first: usize, count: usize, sector_of: &dyn Fn(usize) -> u128| {
                for i in first..first + count {
                    xts.decrypt_sector(
                        &mut h[i * SECTOR..(i + 1) * SECTOR],
                        sector_of(i).to_be_bytes(),
                    );
                }
            };
        // The first two sectors (signatures, magic, main fields) are common to all versions.
        decrypt(&mut h, 0, 2, &|i| i as u128);
        let version = match &h[0x200..0x204] {
            // NCA3: one XTS run, the four FS headers continue the sector numbering.
            b"NCA3" => {
                decrypt(&mut h, 2, 4, &|i| i as u128);
                3
            }
            // NCA2 and the pre-1.0 NCA0: every FS header is its own XTS unit at sector 0.
            b"NCA2" => {
                decrypt(&mut h, 2, 4, &|_| 0);
                2
            }
            b"NCA0" => {
                decrypt(&mut h, 2, 4, &|_| 0);
                0
            }
            b"NCA1" => return Err(Error::Unsupported("NCA1".into())),
            _ => return Err(Error::Integrity("NCA header magic".into())),
        };
        let content_type = match h[0x205] {
            0 => ContentType::Program,
            1 => ContentType::Meta,
            2 => ContentType::Control,
            3 => ContentType::Manual,
            4 => ContentType::Data,
            5 => ContentType::PublicData,
            n => ContentType::Other(n),
        };
        let key_gen = h[0x206].max(h[0x220]).saturating_sub(1);
        let app_idx = h[0x207];
        let rights_id: [u8; 16] = h[0x230..0x240].try_into().unwrap();
        let has_rights = rights_id != [0u8; 16];

        let ctr_key = if has_rights {
            match title_keys.get(&rights_id) {
                Some(enc) => {
                    let mut k = *enc;
                    let tk: [u8; 16] = keys.get(&format!("titlekek_{key_gen:02x}"))?;
                    aes_ecb_decrypt(&tk, &mut k);
                    Some(k)
                }
                None => return Err(Error::MissingKey("ticket for this NCA's rights id".into())),
            }
        } else {
            let kind = match app_idx {
                0 => "application",
                1 => "ocean",
                2 => "system",
                _ => return Err(Error::Unsupported("key area index".into())),
            };
            let mut area = [0u8; 64];
            if version == 0 {
                // NCA0: the key area is a 0x100-byte RSA-OAEP block (key `nca0_key_area_rsa` =
                // private exponent || modulus). Unverified against retail files.
                let kp = keys
                    .get_vec(NCA0_KEY)
                    .ok_or_else(|| Error::MissingKey(NCA0_KEY.into()))?;
                if kp.len() != 0x200 {
                    return Err(Error::KeyFile(format!("key {NCA0_KEY} has wrong length")));
                }
                let pt = oaep_sha256_decrypt(&h[0x300..0x400], &kp[..0x100], &kp[0x100..])?;
                if pt.len() != 64 {
                    return Err(Error::Integrity("NCA0 key area".into()));
                }
                area.copy_from_slice(&pt);
            } else {
                let kek: [u8; 16] = keys.get(&format!("key_area_key_{kind}_{key_gen:02x}"))?;
                area.copy_from_slice(&h[0x300..0x340]);
                aes_ecb_decrypt(&kek, &mut area);
            }
            Some(area[0x20..0x30].try_into().unwrap())
        };

        let mut sections = Vec::new();
        for i in 0..4 {
            let e = 0x240 + i * 0x10;
            let (s, en) = (u32le(&h, e) as u64 * MEDIA, u32le(&h, e + 4) as u64 * MEDIA);
            if en <= s {
                continue;
            }
            let fh = &h[0x400 + i * SECTOR..0x400 + (i + 1) * SECTOR];
            let fs_type = match fh[2] {
                0 => FsType::RomFs,
                1 => FsType::Pfs0,
                _ => return Err(Error::Unsupported("fs type".into())),
            };
            let enc = match fh[4] {
                1 => Enc::None,
                2 => Enc::Xts,
                3 => Enc::Ctr,
                4 => Enc::Bktr,
                _ => return Err(Error::Unsupported("encryption type".into())),
            };
            let mut ctr_hi: [u8; 8] = fh[0x140..0x148].try_into().unwrap();
            ctr_hi.reverse();
            check(s, en - s, src.size())?;
            sections.push(Section {
                index: i,
                start: s,
                end: en,
                fs_type,
                hash_type: fh[3],
                enc,
                hash_data: fh[8..0x100].to_vec(),
                ctr_hi,
            });
        }
        Ok(Self {
            src,
            content_type,
            program_id: u64le(&h, 0x210),
            content_size: u64le(&h, 0x208),
            rights_id,
            sections,
            key_gen,
            ctr_key,
        })
    }

    pub fn master_key_generation(&self) -> u8 {
        self.key_gen
    }

    /// Decrypted view of a section's bytes (offsets relative to section start).
    pub fn section(&self, idx: usize) -> Result<Src> {
        let s = self
            .sections
            .iter()
            .find(|s| s.index == idx)
            .ok_or_else(|| Error::Container("no such section".into()))?;
        let raw = Slice::new(self.src.clone(), s.start, s.end - s.start)?;
        match s.enc {
            Enc::None => Ok(raw),
            Enc::Ctr => Ok(Arc::new(CtrSource {
                raw,
                key: self.ctr_key.unwrap(),
                hi: s.ctr_hi,
                base: s.start,
            })),
            Enc::Xts => Err(Error::Unsupported("AES-XTS sections".into())),
            Enc::Bktr => Err(Error::Unsupported(
                "BKTR (update patch) sections: not yet".into(),
            )),
        }
    }
}

/// AES-CTR random-access decryption. Counter = section ctr (8 bytes) || (absolute offset >> 4).
struct CtrSource {
    raw: Src,
    key: [u8; 16],
    hi: [u8; 8],
    base: u64,
}

impl Source for CtrSource {
    fn size(&self) -> u64 {
        self.raw.size()
    }
    fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<()> {
        check(offset, buf.len() as u64, self.raw.size())?;
        let abs = self.base + offset;
        let pre = (abs % 16) as usize;
        let mut tmp = vec![0u8; pre + buf.len()];
        self.raw.read_at(offset - pre as u64, &mut tmp)?;
        let mut iv = [0u8; 16];
        iv[..8].copy_from_slice(&self.hi);
        let mut c = ctr::Ctr128BE::<Aes128>::new(
            GenericArray::from_slice(&self.key),
            GenericArray::from_slice(&iv),
        );
        c.seek(abs - pre as u64);
        c.apply_keystream(&mut tmp);
        buf.copy_from_slice(&tmp[pre..]);
        Ok(())
    }
    fn map(&self, offset: u64, len: u64, out: &mut Vec<super::io::MapExt>) -> bool {
        check(offset, len, self.raw.size()).is_ok()
            && super::io::map_ctr(&*self.raw, offset, len, self.key, self.hi, self.base, out)
    }
}

/// HierarchicalSha256 (PFS0): validates the master hash over the hash table, which doubles as a
/// key-correctness check. Returns the PFS0 data region.
pub fn pfs0_data(section: &Src, sec: &Section) -> Result<Src> {
    let d = &sec.hash_data;
    let (ht_off, ht_size) = (u64le(d, 0x28), u64le(d, 0x30));
    let (data_off, data_size) = (u64le(d, 0x38), u64le(d, 0x40));
    if ht_size > (1 << 24) {
        return Err(Error::Container("implausible hash table".into()));
    }
    let table = read_vec(&**section, ht_off, ht_size as usize)?;
    if Sha256::digest(&table)[..] != d[..0x20] {
        return Err(Error::Integrity("PFS0 master hash".into()));
    }
    Slice::new(section.clone(), data_off, data_size)
}

/// IVFC (RomFS): checks the hash tree above the data level (master hash and every hash level;
/// cheap, and doubles as a key-correctness check) and returns the data level. Synthetic images
/// without a tree are passed through. Use [`romfs_data_verified`] to also hash every data block.
pub fn romfs_data(section: &Src, sec: &Section) -> Result<Src> {
    romfs_level(section, sec, false)
}

/// Like [`romfs_data`] but streams the whole data level through the hash tree (reads the entire
/// RomFS: seconds for a multi-GB game).
pub fn romfs_data_verified(section: &Src, sec: &Section) -> Result<Src> {
    romfs_level(section, sec, true)
}

fn romfs_level(section: &Src, sec: &Section, full: bool) -> Result<Src> {
    let d = &sec.hash_data;
    match Ivfc::parse(d)? {
        Some(iv) => {
            if full {
                ivfc::verify_data(section, &iv)?;
            } else {
                ivfc::verify_tree(section, &iv)?;
            }
            let l = iv.data();
            Slice::new(section.clone(), l.offset, l.size)
        }
        None => {
            let lvl = 0x10 + 5 * 0x18;
            Slice::new(section.clone(), u64le(d, lvl), u64le(d, lvl + 8))
        }
    }
}
