//! RomFS directory/file tables (read-only, hostile-input safe: bounds, depth and cycle limits).
use super::io::{Slice, Src, read_vec};
use crate::domain::error::{Result, container};

const NONE: u32 = 0xFFFF_FFFF;
const MAX_DEPTH: usize = 64;
const MAX_TABLE: u64 = 1 << 28;

#[derive(Debug, Clone)]
pub struct RomFile {
    pub path: String,
    pub offset: u64, // relative to the RomFS data region start
    pub size: u64,
}

pub struct RomFs {
    src: Src,
    data_off: u64,
    pub files: Vec<RomFile>,
}

fn u32le(b: &[u8], o: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(o..o + 4)?.try_into().ok()?))
}
fn u64le(b: &[u8], o: usize) -> Option<u64> {
    Some(u64::from_le_bytes(b.get(o..o + 8)?.try_into().ok()?))
}

impl RomFs {
    pub fn parse(src: Src) -> Result<Self> {
        let h = read_vec(&*src, 0, 0x50)?;
        let f = |i: usize| u64le(&h, i * 8).unwrap();
        let (dm_off, dm_size, fm_off, fm_size, data_off) = (f(3), f(4), f(7), f(8), f(9));
        if dm_size > MAX_TABLE || fm_size > MAX_TABLE {
            return container("implausible romfs tables");
        }
        let dirs = read_vec(&*src, dm_off, dm_size as usize)?;
        let files = read_vec(&*src, fm_off, fm_size as usize)?;
        let mut out = Vec::new();
        let mut budget = (dm_size + fm_size) as usize / 16 + 1; // each entry is >= 16 bytes
        walk(&dirs, &files, 0, "", 0, &mut out, &mut budget)?;
        Ok(Self {
            src,
            data_off,
            files: out,
        })
    }

    /// The whole RomFS image (header, tables and file data), as stored in the NCA section.
    pub fn image(&self) -> &Src {
        &self.src
    }

    pub fn open(&self, f: &RomFile) -> Result<Src> {
        Slice::new(self.src.clone(), self.data_off + f.offset, f.size)
    }
}

fn name(buf: &[u8], at: usize, len: usize) -> Result<String> {
    let raw = buf
        .get(at..at + len)
        .ok_or(())
        .or_else(|_| container("romfs name out of range"))?;
    let s = std::str::from_utf8(raw).or_else(|_| container("non-utf8 romfs name"))?;
    if s.is_empty() && at != 0 || s == "." || s == ".." || s.contains(['/', '\\', '\0']) {
        return container("unsafe romfs name");
    }
    Ok(s.to_string())
}

fn walk(
    dirs: &[u8],
    files: &[u8],
    dir: u32,
    prefix: &str,
    depth: usize,
    out: &mut Vec<RomFile>,
    budget: &mut usize,
) -> Result<()> {
    if depth > MAX_DEPTH {
        return container("romfs too deep");
    }
    let d = dir as usize;
    let bad = || -> Result<()> { container("romfs directory entry out of range") };
    let (Some(child_dir), Some(child_file)) = (u32le(dirs, d + 8), u32le(dirs, d + 12)) else {
        return bad();
    };
    let mut f = child_file;
    while f != NONE {
        if *budget == 0 {
            return container("romfs cycle or oversized");
        }
        *budget -= 1;
        let o = f as usize;
        let (Some(sib), Some(off), Some(size), Some(nl)) = (
            u32le(files, o + 4),
            u64le(files, o + 8),
            u64le(files, o + 16),
            u32le(files, o + 28),
        ) else {
            return bad();
        };
        out.push(RomFile {
            path: format!("{prefix}{}", name(files, o + 32, nl as usize)?),
            offset: off,
            size,
        });
        f = sib;
    }
    let mut c = child_dir;
    while c != NONE {
        if *budget == 0 {
            return container("romfs cycle or oversized");
        }
        *budget -= 1;
        let o = c as usize;
        let (Some(sib), Some(nl)) = (u32le(dirs, o + 4), u32le(dirs, o + 20)) else {
            return bad();
        };
        let nm = name(dirs, o + 24, nl as usize)?;
        walk(
            dirs,
            files,
            c,
            &format!("{prefix}{nm}/"),
            depth + 1,
            out,
            budget,
        )?;
        c = sib;
    }
    Ok(())
}
