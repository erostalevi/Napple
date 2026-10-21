use super::io::{read_vec, Slice, Src};
use super::pfs::{parse_hfs0, Fs};
use crate::domain::error::{container, Result};

/// Returns XCI partitions (update / normal / secure / logo) as HFS0 file systems.
pub fn parse_xci(src: Src) -> Result<Vec<(String, Fs)>> {
    if src.size() < 0x200 {
        return container("file too small for XCI");
    }
    let head = read_vec(&*src, 0x100, 0x100)?;
    if &head[..4] != b"HEAD" {
        return container("not an XCI (missing HEAD)");
    }
    let off = u64::from_le_bytes(head[0x38..0x40].try_into().unwrap());
    let size = u64::from_le_bytes(head[0x40..0x48].try_into().unwrap());
    let root = parse_hfs0(Slice::new(src, off, size)?)?;
    let mut out = Vec::new();
    for e in &root.entries {
        out.push((e.name.clone(), parse_hfs0(root.open(&e.name)?)?));
    }
    Ok(out)
}
