//! NSO0 executables (main, rtld, sdk, subsdk*): segment decompression and integrity checks.
use crate::domain::error::{container, Error, Result};
use sha2::{Digest, Sha256};

const MAX_SEG: usize = 1 << 29;

#[derive(Debug, Clone)]
pub struct Segment {
    pub mem_offset: u32,
    pub data: Vec<u8>,
}

#[derive(Debug, Clone)]
pub struct Nso {
    pub build_id: [u8; 32],
    pub text: Segment,
    pub rodata: Segment,
    pub data: Segment,
    pub bss_size: u32,
}

fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}

impl Nso {
    pub fn parse(f: &[u8]) -> Result<Self> {
        if f.len() < 0x100 || &f[..4] != b"NSO0" {
            return container("not an NSO");
        }
        let flags = u32le(f, 0xC);
        let mut segs = Vec::new();
        for i in 0..3usize {
            let h = 0x10 + i * 0x10;
            let (file_off, mem_off, dsz) = (u32le(f, h) as usize, u32le(f, h + 4), u32le(f, h + 8) as usize);
            let csz = u32le(f, 0x60 + i * 4) as usize;
            if dsz > MAX_SEG || csz > MAX_SEG || file_off.checked_add(csz).is_none_or(|e| e > f.len()) {
                return container("NSO segment out of range");
            }
            let raw = &f[file_off..file_off + csz];
            let data = if flags & (1 << i) != 0 {
                lz4_flex::block::decompress(raw, dsz).map_err(|_| Error::Container("NSO LZ4 decompression failed".into()))?
            } else {
                raw.to_vec()
            };
            if data.len() != dsz {
                return container("NSO segment size mismatch");
            }
            if flags & (8 << i) != 0 && Sha256::digest(&data)[..] != f[0xA0 + i * 0x20..0xC0 + i * 0x20] {
                return Err(Error::Integrity(format!("NSO segment {i} hash")));
            }
            segs.push(Segment { mem_offset: mem_off, data });
        }
        let mut it = segs.into_iter();
        Ok(Self {
            build_id: f[0x40..0x60].try_into().unwrap(),
            text: it.next().unwrap(),
            rodata: it.next().unwrap(),
            data: it.next().unwrap(),
            bss_size: u32le(f, 0x3C),
        })
    }

    pub fn build_id_hex(&self) -> String {
        self.build_id.iter().map(|b| format!("{b:02x}")).collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn build(text: &[u8], compress: bool, tamper: bool) -> Vec<u8> {
        let mut f = vec![0u8; 0x100];
        f[..4].copy_from_slice(b"NSO0");
        let segs: [&[u8]; 3] = [text, b"ro-data", b"data"];
        let mut flags = 0u32;
        let mut body = Vec::new();
        for (i, s) in segs.iter().enumerate() {
            let stored = if compress { lz4_flex::block::compress(s) } else { s.to_vec() };
            if compress { flags |= 1 << i; }
            flags |= 8 << i;
            let h = 0x10 + i * 0x10;
            f[h..h + 4].copy_from_slice(&((0x100 + body.len()) as u32).to_le_bytes());
            f[h + 4..h + 8].copy_from_slice(&((i * 0x1000) as u32).to_le_bytes());
            f[h + 8..h + 12].copy_from_slice(&(s.len() as u32).to_le_bytes());
            f[0x60 + i * 4..0x64 + i * 4].copy_from_slice(&(stored.len() as u32).to_le_bytes());
            let mut d = Sha256::digest(s).to_vec();
            if tamper && i == 0 { d[0] ^= 1; }
            f[0xA0 + i * 0x20..0xC0 + i * 0x20].copy_from_slice(&d);
            body.extend(stored);
        }
        f[0xC..0x10].copy_from_slice(&flags.to_le_bytes());
        f[0x40] = 0xAB;
        f.extend(body);
        f
    }

    #[test]
    fn parses_compressed_and_plain() {
        for c in [false, true] {
            let n = Nso::parse(&build(&[1, 2, 3, 4, 5, 6, 7, 8], c, false)).unwrap();
            assert_eq!(n.text.data, [1, 2, 3, 4, 5, 6, 7, 8]);
            assert_eq!(n.rodata.data, b"ro-data");
            assert_eq!(n.data.mem_offset, 0x2000);
            assert!(n.build_id_hex().starts_with("ab"));
        }
    }

    #[test]
    fn rejects_tampered_and_garbage() {
        assert!(matches!(Nso::parse(&build(&[0; 8], true, true)), Err(Error::Integrity(_))));
        assert!(Nso::parse(&[0u8; 0x200]).is_err());
        let mut f = build(&[0; 8], false, false);
        f.truncate(0x108);
        assert!(Nso::parse(&f).is_err());
    }
}
