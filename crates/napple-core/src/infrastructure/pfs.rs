//! PFS0 (NSP, ExeFS) and HFS0 (XCI partitions).
use super::io::{Slice, Src, read_vec};
use crate::domain::error::{Result, container};

const MAX_FILES: u32 = 1 << 16;
const MAX_STRTAB: u32 = 1 << 24;

#[derive(Debug, Clone)]
pub struct Entry {
    pub name: String,
    pub offset: u64,
    pub size: u64,
}

pub struct Fs {
    pub src: Src,
    pub entries: Vec<Entry>,
}

impl Fs {
    pub fn open(&self, name: &str) -> Result<Src> {
        match self.entries.iter().find(|e| e.name == name) {
            Some(e) => Slice::new(self.src.clone(), e.offset, e.size),
            None => container(&format!("file not found: {name}")),
        }
    }
    pub fn read(&self, name: &str) -> Result<Vec<u8>> {
        let s = self.open(name)?;
        read_vec(&*s, 0, s.size() as usize)
    }
}

fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn u64le(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}

fn parse(src: Src, magic: &[u8; 4], esz: usize) -> Result<Fs> {
    if src.size() < 16 {
        return container("file system too small");
    }
    let h = read_vec(&*src, 0, 16)?;
    if &h[..4] != magic {
        return container("bad file system magic");
    }
    let (count, strsz) = (u32le(&h, 4), u32le(&h, 8));
    if count > MAX_FILES || strsz > MAX_STRTAB {
        return container("implausible file table");
    }
    let tsz = count as usize * esz;
    let table = read_vec(&*src, 16, tsz)?;
    let strtab = read_vec(&*src, 16 + tsz as u64, strsz as usize)?;
    let data_start = 16 + tsz as u64 + strsz as u64;
    let mut entries = Vec::with_capacity(count as usize);
    for i in 0..count as usize {
        let e = &table[i * esz..];
        let (off, size, so) = (u64le(e, 0), u64le(e, 8), u32le(e, 16) as usize);
        let end = match strtab
            .get(so..)
            .and_then(|s| s.iter().position(|&c| c == 0))
        {
            Some(p) => so + p,
            None => return container("bad string table"),
        };
        let name = std::str::from_utf8(&strtab[so..end]).or_else(|_| container("non-utf8 name"))?;
        if name.is_empty() || name == "." || name == ".." || name.contains(['/', '\\']) {
            return container("unsafe file name in container");
        }
        let abs = data_start
            .checked_add(off)
            .filter(|a| a.checked_add(size).is_some_and(|e| e <= src.size()));
        let Some(abs) = abs else {
            return container("file extends past container");
        };
        entries.push(Entry {
            name: name.to_string(),
            offset: abs,
            size,
        });
    }
    Ok(Fs { src, entries })
}

pub fn parse_pfs0(src: Src) -> Result<Fs> {
    parse(src, b"PFS0", 24)
}
pub fn parse_hfs0(src: Src) -> Result<Fs> {
    parse(src, b"HFS0", 64)
}
