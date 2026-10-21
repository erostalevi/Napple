//! Bounded random-access sources. Every read is range-checked.
use crate::domain::error::{container, Result};
use std::os::unix::fs::FileExt;
use std::sync::Arc;

pub trait Source: Send + Sync {
    fn size(&self) -> u64;
    fn read_at(&self, offset: u64, buf: &mut [u8]) -> Result<()>;
}

pub type Src = Arc<dyn Source>;

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
}

impl FileSource {
    pub fn open(path: &std::path::Path) -> Result<Src> {
        let f = std::fs::File::open(path)?;
        let size = f.metadata()?.len();
        Ok(Arc::new(Self { f, size }))
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
}
