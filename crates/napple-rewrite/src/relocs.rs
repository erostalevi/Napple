//! Relocation tables of a module (RELR, RELA, JMPREL) decoded just enough to know which
//! addresses the loader will store into data, so the rewriter can prove that no pointer
//! enters the middle of a rewritten code window.
//!
//! The rewriter never edits relocations or moves code, so the loader applies the original
//! tables unchanged; this decoder is a safety net, not a transformation.
use crate::error::RewriteError;
use std::collections::BTreeMap;

const DT_PLTRELSZ: i64 = 2;
const DT_SYMTAB: i64 = 6;
const DT_RELA: i64 = 7;
const DT_RELASZ: i64 = 8;
const DT_JMPREL: i64 = 23;
const DT_RELRSZ: i64 = 35;
const DT_RELR: i64 = 36;
const DT_RELRENT: i64 = 37;
const DT_HASH: i64 = 4;

const R_AARCH64_ABS64: u32 = 257;
const R_AARCH64_GLOB_DAT: u32 = 1025;
const R_AARCH64_JUMP_SLOT: u32 = 1026;
const R_AARCH64_RELATIVE: u32 = 1027;

fn rd64(img: &[u8], o: usize) -> Result<u64, RewriteError> {
    img.get(o..o.saturating_add(8))
        .map(|b| u64::from_le_bytes(b.try_into().unwrap()))
        .ok_or_else(|| RewriteError::Relocation(format!("read outside image at {o:#x}")))
}
fn rd32(img: &[u8], o: usize) -> Result<u32, RewriteError> {
    img.get(o..o.saturating_add(4))
        .map(|b| u32::from_le_bytes(b.try_into().unwrap()))
        .ok_or_else(|| RewriteError::Relocation(format!("read outside image at {o:#x}")))
}

/// Decodes DT_RELR into `(where, implicit addend)` pairs; every address must be 8-aligned and
/// inside `[lower, upper)`, no address may repeat and every addend must point into the module.
pub fn decode_relr(
    img: &[u8],
    tags: &BTreeMap<i64, u64>,
    lower: u64,
    upper: u64,
) -> Result<Vec<(u64, u64)>, RewriteError> {
    let present = [DT_RELR, DT_RELRSZ, DT_RELRENT].map(|k| tags.contains_key(&k));
    if !present.iter().any(|p| *p) {
        return Ok(Vec::new());
    }
    if !present.iter().all(|p| *p) || tags[&DT_RELRENT] != 8 || !tags[&DT_RELRSZ].is_multiple_of(8)
    {
        return Err(RewriteError::Relocation("invalid DT_RELR tags".into()));
    }
    let (table, size) = (tags[&DT_RELR] as usize, tags[&DT_RELRSZ] as usize);
    if table.checked_add(size).is_none_or(|e| e > img.len()) {
        return Err(RewriteError::Relocation("RELR table outside image".into()));
    }
    let mut out = Vec::new();
    let mut seen = std::collections::HashSet::new();
    let mut push = |a: u64| -> Result<(), RewriteError> {
        if !a.is_multiple_of(8)
            || a < lower
            || a.checked_add(8)
                .is_none_or(|e| e > upper || e > img.len() as u64)
        {
            return Err(RewriteError::Relocation(format!(
                "RELR relocation outside writable image: {a:#x}"
            )));
        }
        if !seen.insert(a) {
            return Err(RewriteError::Relocation(format!(
                "duplicate RELR relocation: {a:#x}"
            )));
        }
        let addend = rd64(img, a as usize)?;
        if addend >= upper {
            return Err(RewriteError::Relocation(format!(
                "RELR addend outside module: {a:#x}"
            )));
        }
        out.push((a, addend));
        Ok(())
    };
    let mut cursor: Option<u64> = None;
    for o in (table..table + size).step_by(8) {
        let e = rd64(img, o)?;
        if e & 1 == 0 {
            push(e)?;
            cursor = Some(e.saturating_add(8));
        } else {
            let c = cursor.ok_or_else(|| {
                RewriteError::Relocation("RELR bitmap before initial address".into())
            })?;
            for bit in 1..64 {
                if e >> bit & 1 == 1 {
                    push(c.saturating_add((bit as u64 - 1) * 8))?;
                }
            }
            cursor = Some(c.saturating_add(63 * 8));
        }
    }
    Ok(out)
}

/// One relocation the loader will apply.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Reloc {
    pub offset: u64,
    pub kind: u32,
    /// Absolute module-relative value the slot will hold, when it is statically known.
    pub target: Option<u64>,
}

/// All relocations of the module with their module-relative targets (imports have `None`).
///
/// `data_lower` is the first writable offset, `image_len` the end of the module (RELR bounds).
pub fn collect(
    img: &[u8],
    tags: &BTreeMap<i64, u64>,
    data_lower: u64,
    image_len: u64,
) -> Result<Vec<Reloc>, RewriteError> {
    let mut out: Vec<Reloc> = decode_relr(img, tags, data_lower, image_len)?
        .into_iter()
        .map(|(offset, addend)| Reloc {
            offset,
            kind: R_AARCH64_RELATIVE,
            target: Some(addend),
        })
        .collect();
    let nsyms = match tags.get(&DT_HASH) {
        Some(h) => rd32(img, (*h as usize).saturating_add(4)).unwrap_or(0) as usize,
        None => 0,
    };
    for (tab, sz) in [
        (tags.get(&DT_RELA), tags.get(&DT_RELASZ)),
        (tags.get(&DT_JMPREL), tags.get(&DT_PLTRELSZ)),
    ] {
        let (Some(&tab), sz) = (tab, sz.copied().unwrap_or(0)) else {
            continue;
        };
        for i in 0..(sz / 24) as usize {
            let at = (tab as usize).saturating_add(24 * i);
            let (offset, info, addend) = (
                rd64(img, at)?,
                rd64(img, at.saturating_add(8))?,
                rd64(img, at.saturating_add(16))? as i64,
            );
            let (kind, sym) = (info as u32, (info >> 32) as usize);
            let target = match kind {
                R_AARCH64_RELATIVE => Some(addend as u64),
                R_AARCH64_ABS64 | R_AARCH64_GLOB_DAT | R_AARCH64_JUMP_SLOT
                    if sym != 0 && sym < nsyms.max(1) =>
                {
                    let symtab = *tags.get(&DT_SYMTAB).unwrap_or(&0) as usize;
                    let ent = symtab.saturating_add(24 * sym);
                    let (shndx, value) = (
                        img.get(ent.saturating_add(6)..ent.saturating_add(8))
                            .map(|b| u16::from_le_bytes(b.try_into().unwrap()))
                            .unwrap_or(0),
                        rd64(img, ent.saturating_add(8)).unwrap_or(0),
                    );
                    (shndx != 0).then(|| value.wrapping_add(addend as u64))
                }
                _ => None,
            };
            out.push(Reloc {
                offset,
                kind,
                target,
            });
        }
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tags(table: u64, size: u64) -> BTreeMap<i64, u64> {
        BTreeMap::from([(DT_RELR, table), (DT_RELRSZ, size), (DT_RELRENT, 8)])
    }

    #[test]
    fn relr_address_and_bitmap_entries() {
        let mut img = vec![0u8; 0x400];
        // table at 0x200: address 0x100, bitmap 0b1011 -> slots 0x108, 0x110 skipped?, see below
        let entries = [0x100u64, (1 << 1) | (1 << 3) | 1];
        for (i, e) in entries.iter().enumerate() {
            img[0x200 + i * 8..0x208 + i * 8].copy_from_slice(&e.to_le_bytes());
        }
        for a in [0x100u64, 0x108, 0x118] {
            img[a as usize..a as usize + 8].copy_from_slice(&0x40u64.to_le_bytes());
        }
        let r = decode_relr(&img, &tags(0x200, 16), 0x100, 0x400).unwrap();
        assert_eq!(
            r.iter().map(|x| x.0).collect::<Vec<_>>(),
            vec![0x100, 0x108, 0x118]
        );
        assert!(r.iter().all(|x| x.1 == 0x40));
    }

    #[test]
    fn relr_rejects_hostile_tables() {
        let mut img = vec![0u8; 0x400];
        img[0x200..0x208].copy_from_slice(&0x30u64.to_le_bytes()); // below writable range
        assert!(decode_relr(&img, &tags(0x200, 8), 0x100, 0x400).is_err());
        img[0x200..0x208].copy_from_slice(&1u64.to_le_bytes()); // bitmap before address
        assert!(decode_relr(&img, &tags(0x200, 8), 0x100, 0x400).is_err());
        img[0x200..0x208].copy_from_slice(&0x104u64.to_le_bytes()); // unaligned
        assert!(decode_relr(&img, &tags(0x200, 8), 0x100, 0x400).is_err());
        assert!(decode_relr(&img, &tags(0x3F8, 16), 0x100, 0x400).is_err()); // table past the image
        let mut t = tags(0x200, 8);
        t.remove(&DT_RELRSZ);
        assert!(decode_relr(&img, &t, 0x100, 0x400).is_err()); // partial tags
        assert!(
            decode_relr(&img, &BTreeMap::new(), 0x100, 0x400)
                .unwrap()
                .is_empty()
        );
    }

    #[test]
    fn duplicate_relr_slot_is_rejected() {
        let mut img = vec![0u8; 0x400];
        img[0x200..0x208].copy_from_slice(&0x100u64.to_le_bytes());
        img[0x208..0x210].copy_from_slice(&0x100u64.to_le_bytes());
        assert!(decode_relr(&img, &tags(0x200, 16), 0x100, 0x400).is_err());
    }
}
