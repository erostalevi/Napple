//! Streams a decrypted RomFS image to a file (hashing on the way) with a resumable cache.
use crate::error::{BuildError, Result};
use crate::util::{check_cancel, progress};
use napple_core::infrastructure::io::Src;
use sha2::{Digest, Sha256};
use std::io::Write;
use std::path::{Path, PathBuf};

const CHUNK: usize = 8 << 20;

pub struct Streamed {
    pub size: u64,
    pub sha256: String,
}

/// Writes all of `src` to `dst` (via `dst.part`, renamed on success). `report(done, total)` is called per chunk.
pub fn stream_to_file(src: &Src, dst: &Path, mut report: impl FnMut(u64, u64)) -> Result<Streamed> {
    let total = src.size();
    let mut part = dst.as_os_str().to_owned();
    part.push(format!(".part-{}", std::process::id()));
    let part = PathBuf::from(part);
    if let Some(d) = dst.parent() {
        std::fs::create_dir_all(d).map_err(|e| BuildError::io("create dir", e))?;
    }
    let mut run = || -> Result<Streamed> {
        let mut f = std::io::BufWriter::with_capacity(1 << 20, std::fs::File::create(&part).map_err(|e| BuildError::io("create romfs file", e))?);
        let (mut buf, mut off, mut h) = (vec![0u8; CHUNK], 0u64, Sha256::new());
        while off < total {
            check_cancel()?;
            let n = ((total - off) as usize).min(CHUNK);
            src.read_at(off, &mut buf[..n])?;
            h.update(&buf[..n]);
            f.write_all(&buf[..n]).map_err(|e| BuildError::io("write romfs", e))?;
            off += n as u64;
            report(off, total);
        }
        f.flush().map_err(|e| BuildError::io("write romfs", e))?;
        drop(f);
        std::fs::rename(&part, dst).map_err(|e| BuildError::io("rename romfs", e))?;
        Ok(Streamed { size: total, sha256: crate::json::hex(&h.finalize()) })
    };
    let r = run();
    if r.is_err() {
        let _ = std::fs::remove_file(&part);
    }
    r
}

/// A cache entry is a complete file plus a sidecar `<file>.ok` holding `size=<n>` and `sha256=<hex>`.
pub fn cache_lookup(file: &Path, expect_size: u64) -> Option<Streamed> {
    let md = std::fs::metadata(file).ok()?;
    if md.len() != expect_size {
        return None;
    }
    let side = std::fs::read_to_string(sidecar(file)).ok()?;
    let (mut size, mut sha) = (None, None);
    for l in side.lines() {
        match l.split_once('=') {
            Some(("size", v)) => size = v.parse::<u64>().ok(),
            Some(("sha256", v)) if v.len() == 64 && v.bytes().all(|b| b.is_ascii_hexdigit()) => sha = Some(v.to_string()),
            _ => {}
        }
    }
    if size == Some(expect_size) { sha.map(|sha256| Streamed { size: expect_size, sha256 }) } else { None }
}

fn sidecar(file: &Path) -> PathBuf {
    let mut s = file.as_os_str().to_owned();
    s.push(".ok");
    PathBuf::from(s)
}

/// Streams into the cache unless a complete entry with the expected size exists. Returns (info, reused).
pub fn cached_stream(src: &Src, file: &Path, stage: &str, label: &str, lo: f64, hi: f64) -> Result<(Streamed, bool)> {
    if let Some(s) = cache_lookup(file, src.size()) {
        progress(stage, hi, &format!("{label}: reusing cached copy ({} bytes)", s.size));
        return Ok((s, true));
    }
    let _ = std::fs::remove_file(sidecar(file));
    let mut last = -1i64;
    let s = stream_to_file(src, file, |d, t| {
        let pct = lo + (hi - lo) * d as f64 / t.max(1) as f64;
        if (pct * 2.0) as i64 != last {
            last = (pct * 2.0) as i64;
            progress(stage, pct, &format!("{label}: {} / {} MB", d >> 20, t >> 20));
        }
    })?;
    std::fs::write(sidecar(file), format!("size={}\nsha256={}\n", s.size, s.sha256)).map_err(|e| BuildError::io("write cache marker", e))?;
    Ok((s, false))
}

#[cfg(test)]
mod tests {
    use super::*;
    use napple_core::infrastructure::io::BytesSource;
    use std::sync::Arc;

    #[test]
    fn streams_hashes_and_resumes() {
        let dir = std::env::temp_dir().join(format!("napple-stream-{}", std::process::id()));
        let data: Vec<u8> = (0..(CHUNK + 12345)).map(|i| (i % 251) as u8).collect();
        let src: Src = Arc::new(BytesSource(data.clone()));
        let f = dir.join("c/romfs.bin");
        let (s, reused) = cached_stream(&src, &f, "romfs", "t", 0.0, 100.0).unwrap();
        assert!(!reused);
        assert_eq!(std::fs::read(&f).unwrap(), data);
        assert_eq!(s.sha256, crate::json::hex(&Sha256::digest(&data)));
        let (s2, reused) = cached_stream(&src, &f, "romfs", "t", 0.0, 100.0).unwrap();
        assert!(reused && s2.sha256 == s.sha256);
        // a truncated file or a missing marker is not trusted
        std::fs::write(&f, &data[..100]).unwrap();
        assert!(cache_lookup(&f, data.len() as u64).is_none());
        std::fs::write(&f, &data).unwrap();
        std::fs::remove_file(sidecar(&f)).unwrap();
        assert!(cache_lookup(&f, data.len() as u64).is_none());
        let (_, reused) = cached_stream(&src, &f, "romfs", "t", 0.0, 100.0).unwrap();
        assert!(!reused);
        assert!(std::fs::read_dir(f.parent().unwrap()).unwrap().flatten().all(|e| !e.file_name().to_string_lossy().contains(".part")));
        let _ = std::fs::remove_dir_all(dir);
    }
}
