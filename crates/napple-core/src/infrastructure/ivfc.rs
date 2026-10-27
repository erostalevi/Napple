//! IVFC (RomFS) hash-tree verification. The header lists up to six levels; each level is hashed
//! per block (SHA-256, last block zero-padded) into the level above, and level 0 is hashed whole
//! into the master hash. `verify_tree` checks every level except the data level (small, cheap);
//! `verify_data` additionally streams the data level.
use super::io::{Src, check};
use crate::domain::error::{Error, Result};
use sha2::{Digest, Sha256};

const LEVELS_OFF: usize = 0x10;
const LEVEL_STRIDE: usize = 0x18;
const MASTER_OFF: usize = 0xC0;
const MAX_LEVELS: usize = 6;
/// Upper bound for a level held in memory while hashing (index levels are about data/512).
const MAX_BLOCK_LOG2: u32 = 24;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Level {
    pub offset: u64,
    pub size: u64,
    pub block_log2: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Ivfc {
    pub levels: Vec<Level>,
    pub master_hash: [u8; 32],
}

fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn u64le(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}

impl Ivfc {
    /// Parses the IVFC header (`hash_data` of an NCA RomFS section). `Ok(None)` when the header
    /// carries no hash tree (level count below two), which only synthetic images do.
    pub fn parse(d: &[u8]) -> Result<Option<Self>> {
        if d.len() < MASTER_OFF + 32 || &d[..4] != b"IVFC" {
            return Err(Error::Integrity("IVFC magic".into()));
        }
        let (master_size, num) = (u32le(d, 8), u32le(d, 0xC) as usize);
        if num < 2 {
            return Ok(None);
        }
        if num > MAX_LEVELS + 1 || master_size != 32 {
            return Err(Error::Container("implausible IVFC header".into()));
        }
        let levels = (0..num - 1)
            .map(|i| {
                let o = LEVELS_OFF + i * LEVEL_STRIDE;
                Level {
                    offset: u64le(d, o),
                    size: u64le(d, o + 8),
                    block_log2: u32le(d, o + 16),
                }
            })
            .collect();
        Ok(Some(Self {
            levels,
            master_hash: d[MASTER_OFF..MASTER_OFF + 32].try_into().unwrap(),
        }))
    }

    pub fn data(&self) -> Level {
        *self.levels.last().unwrap()
    }
}

fn bad(what: &str) -> Error {
    Error::Integrity(format!("IVFC {what}"))
}

/// Hashes `level` block by block and compares with the digests stored in `above`.
fn check_level(section: &Src, level: Level, above: &[u8]) -> Result<()> {
    if level.block_log2 < 9 || level.block_log2 > MAX_BLOCK_LOG2 {
        return Err(Error::Container("implausible IVFC block size".into()));
    }
    let block = 1u64 << level.block_log2;
    let blocks = level.size.div_ceil(block);
    if (above.len() as u64) < blocks * 32 {
        return Err(bad("hash table too small"));
    }
    let mut buf = vec![0u8; block as usize];
    for i in 0..blocks {
        let n = block.min(level.size - i * block) as usize;
        buf[n..].fill(0);
        section.read_at(level.offset + i * block, &mut buf[..n])?;
        if Sha256::digest(&buf)[..] != above[i as usize * 32..i as usize * 32 + 32] {
            return Err(bad("block hash mismatch"));
        }
    }
    Ok(())
}

fn read_level(section: &Src, l: Level) -> Result<Vec<u8>> {
    if l.size > (1 << 30) {
        return Err(Error::Container("implausible IVFC level".into()));
    }
    check(l.offset, l.size, section.size())?;
    super::io::read_vec(&**section, l.offset, l.size as usize)
}

/// Verifies the master hash and every hash level (all but the data level).
pub fn verify_tree(section: &Src, ivfc: &Ivfc) -> Result<()> {
    let l0 = read_level(section, ivfc.levels[0])?;
    if Sha256::digest(&l0)[..] != ivfc.master_hash {
        return Err(bad("master hash mismatch"));
    }
    let mut above = l0;
    for w in 1..ivfc.levels.len() - 1 {
        let l = ivfc.levels[w];
        check_level(section, l, &above)?;
        above = read_level(section, l)?;
    }
    Ok(())
}

/// Full verification: the tree plus every block of the data level.
pub fn verify_data(section: &Src, ivfc: &Ivfc) -> Result<()> {
    verify_tree(section, ivfc)?;
    let n = ivfc.levels.len();
    if n < 2 {
        return Ok(());
    }
    let above = read_level(section, ivfc.levels[n - 2])?;
    let d = ivfc.data();
    check(d.offset, d.size, section.size())?;
    check_level(section, d, &above)
}

#[cfg(test)]
pub(crate) mod testutil {
    use super::*;

    /// Builds a section image with a valid two-level-below-master IVFC tree over `data`.
    /// Returns (section bytes, 0xF8-byte IVFC header). Block size 0x200.
    pub fn build(data: &[u8]) -> (Vec<u8>, Vec<u8>) {
        let bs = 0x200usize;
        let hash_level = |lvl: &[u8]| -> Vec<u8> {
            let mut out = Vec::new();
            for c in lvl.chunks(bs) {
                let mut b = c.to_vec();
                b.resize(bs, 0);
                out.extend(Sha256::digest(&b));
            }
            out
        };
        let l2 = hash_level(data);
        let l1 = hash_level(&l2);
        let l0 = hash_level(&l1);
        let master = Sha256::digest(&l0);
        let mut img = Vec::new();
        let mut offs = Vec::new();
        for lvl in [&l0, &l1, &l2, &data.to_vec()] {
            offs.push((img.len() as u64, lvl.len() as u64));
            img.extend(lvl.iter());
            img.resize(img.len().div_ceil(bs) * bs, 0);
        }
        let mut hd = vec![0u8; 0xF8];
        hd[..4].copy_from_slice(b"IVFC");
        hd[4..8].copy_from_slice(&0x20000u32.to_le_bytes());
        hd[8..12].copy_from_slice(&32u32.to_le_bytes());
        hd[12..16].copy_from_slice(&5u32.to_le_bytes());
        for (i, (o, s)) in offs.iter().enumerate() {
            let p = LEVELS_OFF + i * LEVEL_STRIDE;
            hd[p..p + 8].copy_from_slice(&o.to_le_bytes());
            hd[p + 8..p + 16].copy_from_slice(&s.to_le_bytes());
            hd[p + 16..p + 20].copy_from_slice(&9u32.to_le_bytes());
        }
        hd[MASTER_OFF..MASTER_OFF + 32].copy_from_slice(&master);
        (img, hd)
    }
}

#[cfg(test)]
mod tests {
    use super::testutil::build;
    use super::*;
    use crate::infrastructure::io::BytesSource;
    use std::sync::Arc;

    fn payload() -> Vec<u8> {
        (0..5000u32).map(|i| (i * 7 + 3) as u8).collect()
    }

    #[test]
    fn valid_tree_and_data_verify() {
        let (img, hd) = build(&payload());
        let s: Src = Arc::new(BytesSource(img));
        let iv = Ivfc::parse(&hd).unwrap().unwrap();
        assert_eq!(iv.levels.len(), 4);
        verify_tree(&s, &iv).unwrap();
        verify_data(&s, &iv).unwrap();
    }

    #[test]
    fn data_corruption_found_only_by_full_check() {
        let (mut img, hd) = build(&payload());
        let iv = Ivfc::parse(&hd).unwrap().unwrap();
        img[iv.data().offset as usize + 4999] ^= 1; // last (partial) block
        let s: Src = Arc::new(BytesSource(img));
        verify_tree(&s, &iv).unwrap();
        assert!(matches!(verify_data(&s, &iv), Err(Error::Integrity(_))));
    }

    #[test]
    fn hash_level_corruption_rejected() {
        for lvl in 0..3 {
            let (mut img, hd) = build(&payload());
            let iv = Ivfc::parse(&hd).unwrap().unwrap();
            img[iv.levels[lvl].offset as usize] ^= 0x80;
            let s: Src = Arc::new(BytesSource(img));
            assert!(
                matches!(verify_tree(&s, &iv), Err(Error::Integrity(_))),
                "level {lvl}"
            );
        }
    }

    #[test]
    fn header_edge_cases() {
        let (_, mut hd) = build(&payload());
        hd[0xC..0x10].copy_from_slice(&0u32.to_le_bytes());
        assert_eq!(Ivfc::parse(&hd).unwrap(), None);
        hd[0xC..0x10].copy_from_slice(&99u32.to_le_bytes());
        assert!(Ivfc::parse(&hd).is_err());
        hd[..4].copy_from_slice(b"XXXX");
        assert!(Ivfc::parse(&hd).is_err());
        let (img, mut hd) = build(&payload());
        hd[LEVELS_OFF + LEVEL_STRIDE + 16..LEVELS_OFF + LEVEL_STRIDE + 20]
            .copy_from_slice(&40u32.to_le_bytes());
        let s: Src = Arc::new(BytesSource(img));
        let iv = Ivfc::parse(&hd).unwrap().unwrap();
        assert!(verify_tree(&s, &iv).is_err());
    }
}
