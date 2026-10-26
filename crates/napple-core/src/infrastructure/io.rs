//! Bounded random-access sources. Every read is range-checked.
use crate::domain::error::{Result, container};
use std::os::unix::fs::FileExt;
use std::sync::Arc;

pub trait Source: Send + Sync {
    fn size(&self) -> u64;
    fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<()>;
    /// Resolves `[offset, offset+len)` down to extents of host files (with the AES-CTR parameters needed to decrypt
    /// them), appended to `out` in order. False when some layer cannot be expressed that way (compressed, in-memory).
    /// Used by `napple build --update-romfs in-place` to describe a RomFS without copying it.
    fn map(&self, _offset: u64, _len: u64, _out: &mut Vec<MapExt>) -> bool {
        false
    }
}

pub type Src = Arc<dyn Source>;

/// One extent of a [`Source::map`]: `len` bytes at `file_off` of `file`; `ctr` = AES-128-CTR (key, IV bytes 0..8,
/// counter byte offset of the extent's first byte; the IV low 8 bytes are big-endian `counter offset >> 4`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MapExt {
    pub len: u64,
    pub file: Arc<std::path::PathBuf>,
    pub file_off: u64,
    pub ctr: Option<([u8; 16], [u8; 8], u64)>,
}

/// Appends `e` to `out`, merging it into the last extent when it continues it exactly.
pub fn push_ext(out: &mut Vec<MapExt>, e: MapExt) {
    if e.len == 0 {
        return;
    }
    if let Some(l) = out.last_mut() {
        let ctr_ok = match (&l.ctr, &e.ctr) {
            (None, None) => true,
            (Some((k1, h1, c1)), Some((k2, h2, c2))) => k1 == k2 && h1 == h2 && c1 + l.len == *c2,
            _ => false,
        };
        if ctr_ok && l.file == e.file && l.file_off + l.len == e.file_off {
            l.len += e.len;
            return;
        }
    }
    out.push(e);
}

/// Maps `src[offset..offset+len)` and applies AES-CTR (`key`, `hi`, counter offset `ctr_base + offset`) to the
/// plaintext extents found (the layer under a CTR layer must be unencrypted).
pub fn map_ctr(
    src: &dyn Source,
    offset: u64,
    len: u64,
    key: [u8; 16],
    hi: [u8; 8],
    ctr_base: u64,
    out: &mut Vec<MapExt>,
) -> bool {
    let mut tmp = Vec::new();
    if !src.map(offset, len, &mut tmp) {
        return false;
    }
    let mut pos = offset;
    for mut e in tmp {
        if e.ctr.is_some() {
            return false;
        }
        e.ctr = Some((key, hi, ctr_base + pos));
        pos += e.len;
        push_ext(out, e);
    }
    true
}

pub fn check(off: u64, len: u64, size: u64) -> Result<()> {
    match off.checked_add(len) {
        Some(end) if end <= size => Ok(()),
        _ => container("read out of bounds"),
    }
}

pub fn read_vec(s: &dyn Source, off: u64, len: usize) -> Result<Vec<u8>> {
    check(off, len as u64, s.size())?;
    let mut v = vec![0u8; len];
    s.read_at(off, &mut v)?;
    Ok(v)
}

pub struct FileSource {
    f: std::fs::File,
    size: u64,
    path: Arc<std::path::PathBuf>,
}

impl FileSource {
    pub fn open(path: &std::path::Path) -> Result<Src> {
        let f = std::fs::File::open(path)?;
        let size = f.metadata()?.len();
        let path = Arc::new(std::fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf()));
        Ok(Arc::new(Self { f, size, path }))
    }
}

impl Source for FileSource {
    fn size(&self) -> u64 {
        self.size
    }
    fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<()> {
        check(offset, buf.len() as u64, self.size)?;
        self.f.read_exact_at(buf, offset)?;
        Ok(())
    }
    fn map(&self, offset: u64, len: u64, out: &mut Vec<MapExt>) -> bool {
        if check(offset, len, self.size).is_err() {
            return false;
        }
        push_ext(
            out,
            MapExt {
                len,
                file: self.path.clone(),
                file_off: offset,
                ctr: None,
            },
        );
        true
    }
}

pub struct BytesSource(pub Vec<u8>);

impl Source for BytesSource {
    fn size(&self) -> u64 {
        self.0.len() as u64
    }
    fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<()> {
        check(offset, buf.len() as u64, self.size())?;
        buf.copy_from_slice(&self.0[offset as usize..offset as usize + buf.len()]);
        Ok(())
    }
}

pub struct Slice {
    parent: Src,
    off: u64,
    len: u64,
}

impl Slice {
    // Hands back the shared `Src` handle, which is how every caller holds a source.
    #[allow(clippy::new_ret_no_self)]
    pub fn new(parent: Src, off: u64, len: u64) -> Result<Src> {
        check(off, len, parent.size())?;
        Ok(Arc::new(Self { parent, off, len }))
    }
}

impl Source for Slice {
    fn size(&self) -> u64 {
        self.len
    }
    fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<()> {
        check(offset, buf.len() as u64, self.len)?;
        self.parent.read_at(self.off + offset, buf)
    }
    fn map(&self, offset: u64, len: u64, out: &mut Vec<MapExt>) -> bool {
        check(offset, len, self.len).is_ok() && self.parent.map(self.off + offset, len, out)
    }
}
