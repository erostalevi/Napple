//! `romfs.map` (napple build --update-romfs in-place): a RomFS image described as extents of the game files plus the
//! AES-128-CTR parameters to decrypt them, so a development data directory needs no RomFS copy (the runtime's
//! services/romfs_map.c decrypts on read). The map holds content keys and absolute host paths: development only.
//!
//! Format (little endian): "RMP1", u32 file count, u64 extent count, u64 image size; per file u32 length + UTF-8
//! path; per extent 64 bytes: u64 image offset, u64 length, u64 file offset, u64 counter offset, u32 file index,
//! u32 flags (bit 0: AES-CTR), u8 iv_hi[8], u8 key[16]. Extents are sorted, contiguous and start at 0.
use crate::error::{BuildError, Result};
use aes::cipher::{generic_array::GenericArray, KeyIvInit, StreamCipher, StreamCipherSeek};
use aes::Aes128;
use napple_core::infrastructure::io::{MapExt, Src};
use std::io::Write;
use std::os::unix::fs::FileExt;
use std::path::Path;

/// Reads `[off, off+buf.len())` of the image through the extents (the runtime's algorithm; used to verify a map).
pub fn read_through(exts: &[MapExt], starts: &[u64], off: u64, buf: &mut [u8]) -> std::io::Result<()> {
    let (mut pos, mut done) = (off, 0usize);
    while done < buf.len() {
        let i = starts.partition_point(|&s| s <= pos) - 1;
        let e = &exts[i];
        let skip = pos - starts[i];
        let n = ((e.len - skip) as usize).min(buf.len() - done);
        let f = std::fs::File::open(&*e.file)?;
        let dst = &mut buf[done..done + n];
        match &e.ctr {
            None => f.read_exact_at(dst, e.file_off + skip)?,
            Some((key, hi, c0)) => {
                let c = c0 + skip;
                let pre = (c % 16) as usize;
                let mut tmp = vec![0u8; pre + n];
                f.read_exact_at(&mut tmp, e.file_off + skip - pre as u64)?;
                let mut iv = [0u8; 16];
                iv[..8].copy_from_slice(hi);
                let mut k = ctr::Ctr128BE::<Aes128>::new(GenericArray::from_slice(key), GenericArray::from_slice(&iv));
                k.seek(c - pre as u64);
                k.apply_keystream(&mut tmp);
                dst.copy_from_slice(&tmp[pre..]);
            }
        }
        pos += n as u64;
        done += n;
    }
    Ok(())
}

/// Writes `path` for `src`; returns the extent count. Fails when `src` cannot be mapped or a sample read through the
/// map differs from `src`.
pub fn write_map(src: &Src, path: &Path) -> Result<usize> {
    let size = src.size();
    let mut exts = Vec::new();
    if !src.map(0, size, &mut exts) {
        return Err(BuildError::new("romfs", "this RomFS cannot be read in place (compressed or in-memory container layer); use --update-romfs materialize"));
    }
    let mut starts = Vec::with_capacity(exts.len());
    let mut at = 0u64;
    for e in &exts {
        starts.push(at);
        at += e.len;
    }
    if at != size {
        return Err(BuildError::new("romfs", format!("in-place map covers {at} of {size} bytes")));
    }
    // verification: the RomFS header, extent boundaries and pseudo-random ranges, compared with the source
    let mut probes: Vec<u64> = vec![0];
    for (i, s) in starts.iter().enumerate().step_by((starts.len() / 64).max(1)) {
        probes.push(s.saturating_sub(7));
        let _ = i;
    }
    let mut x = 0x9e37_79b9_7f4a_7c15u64 ^ size;
    for _ in 0..64 {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        probes.push(x % size.max(1));
    }
    for p in probes {
        let n = (size - p).min(0x3000) as usize;
        let (mut a, mut b) = (vec![0u8; n], vec![0u8; n]);
        src.read_at(p, &mut a).map_err(|e| BuildError::new("romfs", format!("verify read: {e}")))?;
        read_through(&exts, &starts, p, &mut b).map_err(|e| BuildError::io("verify romfs.map", e))?;
        if a != b {
            return Err(BuildError::new("romfs", format!("in-place map differs from the RomFS at offset {p:#x}")));
        }
    }
    let mut files: Vec<std::sync::Arc<std::path::PathBuf>> = Vec::new();
    let mut f = std::io::BufWriter::new(std::fs::File::create(path).map_err(|e| BuildError::io("create romfs.map", e))?);
    let mut idx = Vec::with_capacity(exts.len());
    for e in &exts {
        let i = match files.iter().position(|p| p == &e.file) {
            Some(i) => i,
            None => {
                files.push(e.file.clone());
                files.len() - 1
            }
        };
        idx.push(i as u32);
    }
    let w = (|| -> std::io::Result<()> {
        f.write_all(b"RMP1")?;
        f.write_all(&(files.len() as u32).to_le_bytes())?;
        f.write_all(&(exts.len() as u64).to_le_bytes())?;
        f.write_all(&size.to_le_bytes())?;
        for p in &files {
            let b = p.to_string_lossy();
            f.write_all(&(b.len() as u32).to_le_bytes())?;
            f.write_all(b.as_bytes())?;
        }
        for ((e, s), i) in exts.iter().zip(&starts).zip(&idx) {
            let (key, hi, c, flags) = match &e.ctr {
                Some((k, h, c)) => (*k, *h, *c, 1u32),
                None => ([0u8; 16], [0u8; 8], 0, 0),
            };
            for v in [*s, e.len, e.file_off, c] {
                f.write_all(&v.to_le_bytes())?;
            }
            f.write_all(&i.to_le_bytes())?;
            f.write_all(&flags.to_le_bytes())?;
            f.write_all(&hi)?;
            f.write_all(&key)?;
        }
        f.flush()
    })();
    w.map_err(|e| BuildError::io("write romfs.map", e))?;
    Ok(exts.len())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bktr::CtrSource;
    use napple_core::infrastructure::io::{FileSource, Slice, Source};
    use std::sync::Arc;

    #[test]
    fn map_reads_back_like_the_source() {
        let dir = std::env::temp_dir().join(format!("napple-romfs-map-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let file = dir.join("game.bin");
        let data: Vec<u8> = (0..0x9000u32).map(|i| (i * 7 + (i >> 8)) as u8).collect();
        std::fs::write(&file, &data).unwrap();
        let raw = Slice::new(FileSource::open(&file).unwrap(), 0x1000, 0x7000).unwrap();
        let ctr: Src = Arc::new(CtrSource::new(raw, [7u8; 16], [1, 2, 3, 4, 5, 6, 7, 8], 0x1000));
        let view = Slice::new(ctr, 0x123, 0x5000).unwrap();
        let out = dir.join("romfs.map");
        assert_eq!(write_map(&view, &out).unwrap(), 1);
        let mut exts = Vec::new();
        assert!(view.map(0x10, 0x2000, &mut exts));
        let (mut a, mut b) = (vec![0u8; 0x2000], vec![0u8; 0x2000]);
        view.read_at(0x10, &mut a).unwrap();
        read_through(&exts, &[0], 0, &mut b).unwrap();
        assert_eq!(a, b);
        let bytes = std::fs::read(&out).unwrap();
        assert_eq!(&bytes[..4], b"RMP1");
        let _ = std::fs::remove_dir_all(&dir);
    }
}
