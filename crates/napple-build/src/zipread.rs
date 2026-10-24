//! Minimal ZIP reader for the firmware archive (central directory, stored and deflate entries, no zip64).
//! Stored entries are served as bounded slices of the file (no copy); deflate entries are inflated into memory.
use crate::error::{BuildError, Result};
use napple_core::infrastructure::io::{read_vec, BytesSource, FileSource, Slice, Src};
use std::io::Read;
use std::path::Path;

const MAX_INFLATED: u64 = 1 << 30;

#[derive(Debug, Clone)]
pub struct ZipEntry {
    pub name: String,
    pub method: u16,
    pub csize: u64,
    pub usize: u64,
    pub local_off: u64,
}

pub struct ZipArchive {
    src: Src,
    pub entries: Vec<ZipEntry>,
}

fn u16le(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([b[o], b[o + 1]])
}
fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn bad(m: &str) -> BuildError {
    BuildError::new("firmware", format!("zip: {m}"))
}

impl ZipArchive {
    pub fn open(path: &Path) -> Result<Self> {
        Self::from_source(FileSource::open(path)?)
    }

    pub fn from_source(src: Src) -> Result<Self> {
        let size = src.size();
        if size < 22 {
            return Err(bad("file too small"));
        }
        let tail_len = size.min(22 + 0xFFFF) as usize;
        let tail = read_vec(&*src, size - tail_len as u64, tail_len)?;
        let eocd = (0..=tail_len - 22).rev().find(|&i| tail[i..i + 4] == [0x50, 0x4B, 5, 6]).ok_or_else(|| bad("end of central directory not found"))?;
        let e = &tail[eocd..];
        let (count, cd_size, cd_off) = (u16le(e, 10) as usize, u32le(e, 12) as u64, u32le(e, 16) as u64);
        if count == 0xFFFF || cd_size == 0xFFFF_FFFF || cd_off == 0xFFFF_FFFF {
            return Err(bad("zip64 archives are not supported"));
        }
        if cd_off.checked_add(cd_size).is_none_or(|end| end > size) || cd_size > (1 << 26) {
            return Err(bad("central directory out of range"));
        }
        let cd = read_vec(&*src, cd_off, cd_size as usize)?;
        let (mut entries, mut o) = (Vec::with_capacity(count), 0usize);
        for _ in 0..count {
            if o + 46 > cd.len() || cd[o..o + 4] != [0x50, 0x4B, 1, 2] {
                return Err(bad("corrupt central directory"));
            }
            let (method, csize, usz, nl, el, cl, lho) =
                (u16le(&cd, o + 10), u32le(&cd, o + 20) as u64, u32le(&cd, o + 24) as u64, u16le(&cd, o + 28) as usize, u16le(&cd, o + 30) as usize, u16le(&cd, o + 32) as usize, u32le(&cd, o + 42) as u64);
            let name = cd.get(o + 46..o + 46 + nl).ok_or_else(|| bad("corrupt central directory"))?;
            entries.push(ZipEntry { name: String::from_utf8_lossy(name).into_owned(), method, csize, usize: usz, local_off: lho });
            o += 46 + nl + el + cl;
        }
        Ok(Self { src, entries })
    }

    /// Data of an entry as a random-access source.
    pub fn open_entry(&self, e: &ZipEntry) -> Result<Src> {
        let lh = read_vec(&*self.src, e.local_off, 30)?;
        if lh[..4] != [0x50, 0x4B, 3, 4] {
            return Err(bad("bad local header"));
        }
        let data_off = e.local_off + 30 + u16le(&lh, 26) as u64 + u16le(&lh, 28) as u64;
        let raw = Slice::new(self.src.clone(), data_off, e.csize).map_err(|_| bad("entry extends past the archive"))?;
        match e.method {
            0 => Ok(raw),
            8 => {
                if e.usize > MAX_INFLATED {
                    return Err(bad("entry too large"));
                }
                let comp = read_vec(&*raw, 0, e.csize as usize)?;
                let mut out = Vec::with_capacity(e.usize as usize);
                flate2::read::DeflateDecoder::new(&comp[..]).take(MAX_INFLATED).read_to_end(&mut out).map_err(|x| bad(&format!("inflate failed: {x}")))?;
                if out.len() as u64 != e.usize {
                    return Err(bad("inflated size mismatch"));
                }
                Ok(std::sync::Arc::new(BytesSource(out)))
            }
            m => Err(bad(&format!("unsupported compression method {m}"))),
        }
    }
}

#[cfg(test)]
pub(crate) fn make_zip(files: &[(&str, &[u8], bool)]) -> Vec<u8> {
    use std::io::Write;
    let (mut out, mut cd) = (Vec::new(), Vec::new());
    for (name, data, deflate) in files {
        let comp = if *deflate {
            let mut e = flate2::write::DeflateEncoder::new(Vec::new(), flate2::Compression::fast());
            e.write_all(data).unwrap();
            e.finish().unwrap()
        } else {
            data.to_vec()
        };
        let lho = out.len() as u32;
        let method = if *deflate { 8u16 } else { 0 };
        out.extend([0x50, 0x4B, 3, 4, 20, 0, 0, 0]);
        out.extend(method.to_le_bytes());
        out.extend([0u8; 8]); // time, date, crc (not checked)
        out.extend((comp.len() as u32).to_le_bytes());
        out.extend((data.len() as u32).to_le_bytes());
        out.extend((name.len() as u16).to_le_bytes());
        out.extend([0, 0]);
        out.extend(name.as_bytes());
        out.extend(&comp);
        cd.extend([0x50, 0x4B, 1, 2, 20, 0, 20, 0, 0, 0]);
        cd.extend(method.to_le_bytes());
        cd.extend([0u8; 8]);
        cd.extend((comp.len() as u32).to_le_bytes());
        cd.extend((data.len() as u32).to_le_bytes());
        cd.extend((name.len() as u16).to_le_bytes());
        cd.extend([0u8; 12]);
        cd.extend(lho.to_le_bytes());
        cd.extend(name.as_bytes());
    }
    let cd_off = out.len() as u32;
    out.extend(&cd);
    out.extend([0x50, 0x4B, 5, 6, 0, 0, 0, 0]);
    out.extend((files.len() as u16).to_le_bytes());
    out.extend((files.len() as u16).to_le_bytes());
    out.extend((cd.len() as u32).to_le_bytes());
    out.extend(cd_off.to_le_bytes());
    out.extend([0, 0]);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_stored_and_deflated_entries() {
        let big: Vec<u8> = (0..5000u32).map(|i| (i % 7) as u8).collect();
        let z = make_zip(&[("a.nca", b"hello", false), ("b.nca", &big, true)]);
        let a = ZipArchive::from_source(std::sync::Arc::new(BytesSource(z))).unwrap();
        assert_eq!(a.entries.len(), 2);
        let s = a.open_entry(&a.entries[0]).unwrap();
        assert_eq!(read_vec(&*s, 0, 5).unwrap(), b"hello");
        let s = a.open_entry(&a.entries[1]).unwrap();
        assert_eq!(read_vec(&*s, 0, 5000).unwrap(), big);
    }

    #[test]
    fn hostile_zips_error_cleanly() {
        for n in [0usize, 10, 22] {
            assert!(ZipArchive::from_source(std::sync::Arc::new(BytesSource(vec![0; n]))).is_err());
        }
        let mut z = make_zip(&[("a", b"x", false)]);
        let l = z.len();
        z[l - 6..l - 2].copy_from_slice(&0x7FFF_FFFFu32.to_le_bytes()); // cd offset out of range
        assert!(ZipArchive::from_source(std::sync::Arc::new(BytesSource(z))).is_err());
    }
}
