//! NTB2 module tables for AArch32 (ELF32, DT_REL) titles such as Mario Kart 8 Deluxe.
//!
//! The record layout is NTB1's (docs/runtime-modules.md) so the runtime tables reader is shared; only the magic, the relocation kinds (ARM
//! numbers 2 ABS32, 21 GLOB_DAT, 22 JUMP_SLOT, 23 RELATIVE) and the meaning of a few fields differ. REL entries carry their addend in the
//! relocated word, so it is read here and stored in the addend field; GLOB_DAT / JUMP_SLOT have addend 0. There is no rewriter output for these
//! modules: the image is the plain NSO image (text, ro, data at their memory offsets, then bss), `patch_off` and `key_page_off` equal its end.
//! Every read is bounds-checked: modules are untrusted input.
use crate::error::{BuildError, Result};
use crate::tables::TableStats;
use napple_core::infrastructure::nso::Nso;
use std::collections::{BTreeMap, HashMap, HashSet};

pub const R_ARM_ABS32: u32 = 2;
pub const R_ARM_GLOB_DAT: u32 = 21;
pub const R_ARM_JUMP_SLOT: u32 = 22;
pub const R_ARM_RELATIVE: u32 = 23;
const KINDS: [u32; 4] = [R_ARM_ABS32, R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT, R_ARM_RELATIVE];
const MAX_IMAGE: usize = 1 << 29;

fn err(name: &str, m: impl std::fmt::Display) -> BuildError {
    BuildError::new("tables32", format!("{name}: {m}"))
}
fn u32_at(b: &[u8], o: usize) -> Option<u32> {
    b.get(o..o.checked_add(4)?)?.try_into().ok().map(u32::from_le_bytes)
}
fn u16_at(b: &[u8], o: usize) -> Option<u16> {
    b.get(o..o.checked_add(2)?)?.try_into().ok().map(u16::from_le_bytes)
}

/// The module image: segments at their memory offsets, bss after them, rounded up to a page.
pub fn flat_image(name: &str, nso: &Nso) -> Result<Vec<u8>> {
    let segs = [&nso.text, &nso.rodata, &nso.data];
    let mut end = 0usize;
    for s in segs {
        let (o, l) = (s.mem_offset as usize, s.data.len());
        if o < end {
            return Err(err(name, "segments overlap"));
        }
        end = o + l;
    }
    let len = end.checked_add(nso.bss_size as usize).filter(|n| *n <= MAX_IMAGE).ok_or_else(|| err(name, "module image too large"))?;
    let len = (len + 0xFFF) & !0xFFF;
    let mut img = vec![0u8; len];
    for s in segs {
        img[s.mem_offset as usize..s.mem_offset as usize + s.data.len()].copy_from_slice(&s.data);
    }
    Ok(img)
}

/// Dynamic tags of an ELF32 image (8-byte entries; first occurrence wins). `None` when the module does not look like ELF32 with REL relocations.
fn dynamic32(image: &[u8]) -> Option<BTreeMap<u32, u32>> {
    let mod0 = u32_at(image, 4)? as usize;
    if image.get(mod0..mod0 + 4)? != b"MOD0" {
        return None;
    }
    let dyn_off = (mod0 as i64 + u32_at(image, mod0 + 4)? as i32 as i64) as usize;
    let mut tags = BTreeMap::new();
    for i in 0..4096usize {
        let (t, v) = (u32_at(image, dyn_off + i * 8)?, u32_at(image, dyn_off + i * 8 + 4)?);
        if t == 0 {
            return Some(tags);
        }
        tags.entry(t).or_insert(v);
    }
    None
}

/// Is this module AArch32 (ELF32, DT_REL, no DT_RELA)? Used to route a module away from the AArch64 rewriter.
/// `NAPPLE_A32_RECOMPILE=1`: after the ELF32 modules of one version are written to `mdir`, lifts them to AArch64 (`napple rewrite a32`, crates/napple-rewrite/src/a32.rs)
/// and installs `a32jit.dylib` + `a32jit.manifest` next to them. The runtime uses the image when it is there and matches the modules, and interprets everything else
/// (mixed mode), so skipping this step only costs speed. iOS packaging (not done here): the dylib goes into the app's Frameworks folder and is signed with the app;
/// it contains no run-time code generation.
pub fn recompile_if_requested(mdir: &std::path::Path) -> Result<()> {
    if std::env::var("NAPPLE_A32_RECOMPILE").map(|v| v != "0").unwrap_or(false) {
        let is32 = std::fs::read(mdir.join("main.tables")).map(|t| t.starts_with(b"NTB2")).unwrap_or(false);
        if !is32 {
            return Ok(());
        }
        let work = std::env::temp_dir().join(format!("napple-a32jit-{}", std::process::id()));
        let r = napple_rewrite::a32::recompile(mdir, &work).map_err(|e| BuildError::new("a32jit", e.to_string()));
        let _ = std::fs::remove_dir_all(&work);
        r?;
    }
    Ok(())
}

pub fn is_elf32(nso: &Nso) -> bool {
    let Ok(img) = flat_image("probe", nso) else { return false };
    dynamic32(&img).is_some_and(|t| t.contains_key(&17) && !t.contains_key(&7))
}

/// Builds the plain image and the NTB2 tables of one AArch32 module.
pub fn build_module32(name: &str, nso: &Nso, nso_file: &[u8]) -> Result<(Vec<u8>, Vec<u8>, TableStats)> {
    let image = flat_image(name, nso)?;
    let tags = dynamic32(&image).ok_or_else(|| err(name, "no ELF32 dynamic table"))?;
    let tag = |t: u32| tags.get(&t).copied();
    let (text_end, ro_off, data_off) = (nso.text.data.len() as u64, nso.rodata.mem_offset as u64, nso.data.mem_offset as u64);
    let nsym = if let Some(h) = tag(4) {
        u32_at(&image, h as usize + 4).ok_or_else(|| err(name, "DT_HASH outside the image"))? as usize
    } else {
        let (off, size) = (u32_at(nso_file, 0x98), u32_at(nso_file, 0x9C));
        let (Some(off), Some(size)) = (off, size) else { return Err(err(name, "NSO header truncated")) };
        if Some(ro_off as u32 + off) != tag(6) || size % 16 != 0 {
            return Err(err(name, "dynsym extent disagrees with the NSO header"));
        }
        (size / 16) as usize
    };
    if nsym > (1 << 24) {
        return Err(err(name, "implausible symbol count"));
    }
    if tag(19).is_some_and(|v| v != 8) || tag(11).is_some_and(|v| v != 16) {
        return Err(err(name, "unsupported dynamic entry sizes (expected REL with 8-byte entries and 16-byte symbols)"));
    }
    let (symtab, strtab_addr, strsz) = (
        tag(6).ok_or_else(|| err(name, "DT_SYMTAB missing"))? as usize,
        tag(5).ok_or_else(|| err(name, "DT_STRTAB missing"))? as usize,
        tag(10).ok_or_else(|| err(name, "DT_STRSZ missing"))? as usize,
    );
    let mut strtab = vec![0u8];
    let mut offsets: HashMap<Vec<u8>, u32> = HashMap::new();
    let mut syms = Vec::with_capacity(nsym);
    for i in 0..nsym {
        let e = symtab + 16 * i;
        let (Some(no), Some(value), Some(size), Some(info), Some(shndx)) = (u32_at(&image, e), u32_at(&image, e + 4), u32_at(&image, e + 8), image.get(e + 12).copied(), u16_at(&image, e + 14)) else {
            return Err(err(name, "symbol table outside the image"));
        };
        let start = strtab_addr + no as usize;
        let limit = (strtab_addr + strsz).min(image.len());
        let end = image.get(start..limit).and_then(|s| s.iter().position(|&c| c == 0)).ok_or_else(|| err(name, "unterminated symbol name"))? + start;
        let s = image[start..end].to_vec();
        let off = *offsets.entry(s.clone()).or_insert_with(|| {
            let o = strtab.len() as u32;
            strtab.extend(&s);
            strtab.push(0);
            o
        });
        syms.push((off, info >> 4, info & 15, (shndx != 0) as u8, value as u64, size as u64));
    }
    let mut rels: Vec<(u64, i64, u32, u32)> = Vec::new();
    let mut seen: HashSet<u64> = HashSet::new();
    for (addr, size) in [(tag(17), tag(18).unwrap_or(0)), (tag(23), tag(2).unwrap_or(0))] {
        let Some(addr) = addr.filter(|a| *a != 0) else { continue };
        if size % 8 != 0 {
            return Err(err(name, "relocation table size is not a multiple of 8"));
        }
        for off in (addr..addr + size).step_by(8) {
            let (Some(wh), Some(info)) = (u32_at(&image, off as usize), u32_at(&image, off as usize + 4)) else {
                return Err(err(name, "relocation table outside the image"));
            };
            let (kind, si) = (info & 0xFF, info >> 8);
            if !KINDS.contains(&kind) || (kind != R_ARM_RELATIVE && si as usize >= nsym) {
                return Err(err(name, format!("relocation kind {kind} symbol {si}")));
            }
            if wh as usize + 4 > image.len() || wh % 4 != 0 {
                return Err(err(name, format!("relocation at {wh:#x} outside the image or unaligned")));
            }
            if !seen.insert(wh as u64) {
                return Err(err(name, "duplicate relocation"));
            }
            let addend = match kind {
                R_ARM_RELATIVE | R_ARM_ABS32 => u32_at(&image, wh as usize).unwrap() as i64,
                _ => 0,
            };
            rels.push((wh as u64, addend, kind, if kind == R_ARM_RELATIVE { 0 } else { si }));
        }
    }
    let build_id: [u8; 32] = nso.build_id;
    let end = image.len() as u64;
    let mut out = Vec::with_capacity(0x100 + syms.len() * 24 + rels.len() * 24 + strtab.len());
    out.extend(b"NTB2");
    for v in [1u32, syms.len() as u32, rels.len() as u32, 0, strtab.len() as u32] {
        out.extend(v.to_le_bytes());
    }
    for v in [end, text_end, ro_off, data_off, end, end] {
        out.extend(v.to_le_bytes());
    }
    for t in [12, 13, 25, 27, 26, 28] {
        out.extend((tag(t).unwrap_or(0) as u64).to_le_bytes());
    }
    out.extend(build_id);
    for (n, b, t, d, v, s) in &syms {
        out.extend(n.to_le_bytes());
        out.extend([*b, *t, *d, 0]);
        out.extend(v.to_le_bytes());
        out.extend(s.to_le_bytes());
    }
    for (w, a, k, si) in &rels {
        out.extend(w.to_le_bytes());
        out.extend(a.to_le_bytes());
        out.extend(k.to_le_bytes());
        out.extend(si.to_le_bytes());
    }
    out.extend(&strtab);
    Ok((image, out, TableStats { symbols: syms.len(), relocs: rels.len(), gates: 0, build_id }))
}

#[cfg(test)]
mod tests {
    use super::*;
    use napple_core::infrastructure::nso::Segment;

    fn put(b: &mut [u8], o: usize, v: u32) {
        b[o..o + 4].copy_from_slice(&v.to_le_bytes());
    }

    /// A tiny ELF32 module: MOD0 at 8, one exported function, two relocations (RELATIVE and ABS32 against it) and one JUMP_SLOT import.
    fn tiny() -> (Nso, Vec<u8>) {
        let mut t = vec![0u8; 0x1000];
        put(&mut t, 4, 8);
        t[8..12].copy_from_slice(b"MOD0");
        put(&mut t, 12, 0x100 - 8); // dynamic at 0x100
        let dynv = [(4u32, 0x200u32), (5, 0x260), (6, 0x220), (10, 0x20), (11, 16), (17, 0x300), (18, 16), (19, 8), (23, 0x320), (2, 8), (20, 17), (0, 0)];
        for (i, (k, v)) in dynv.iter().enumerate() {
            put(&mut t, 0x100 + 8 * i, *k);
            put(&mut t, 0x100 + 8 * i + 4, *v);
        }
        put(&mut t, 0x200 + 4, 3); // DT_HASH: nchain = 3 symbols
        // symbols: null, "func" (defined, FUNC global), "imp" (undefined)
        put(&mut t, 0x220 + 16, 1);
        put(&mut t, 0x220 + 16 + 4, 0x40);
        put(&mut t, 0x220 + 16 + 8, 4);
        t[0x220 + 16 + 12] = (1 << 4) | 2;
        t[0x220 + 16 + 14] = 1;
        put(&mut t, 0x220 + 32, 6);
        t[0x220 + 32 + 12] = 1 << 4;
        t[0x260..0x260 + 9].copy_from_slice(b"\0func\0imp");
        // REL: RELATIVE at 0x500 (addend 0x40 in place), ABS32 at 0x504 sym 1 (addend 8 in place); JMPREL: JUMP_SLOT at 0x508 sym 2
        put(&mut t, 0x300, 0x500);
        put(&mut t, 0x304, 23);
        put(&mut t, 0x308, 0x504);
        put(&mut t, 0x30C, (1 << 8) | 2);
        put(&mut t, 0x320, 0x508);
        put(&mut t, 0x324, (2 << 8) | 22);
        put(&mut t, 0x500, 0x40);
        put(&mut t, 0x504, 8);
        let nso = Nso { build_id: [7; 32], text: Segment { mem_offset: 0, data: t }, rodata: Segment { mem_offset: 0x1000, data: vec![0; 0x100] }, data: Segment { mem_offset: 0x2000, data: vec![0; 0x100] }, bss_size: 0x40 };
        (nso, vec![0u8; 0x100])
    }

    #[test]
    fn tiny_module_tables() {
        let (nso, file) = tiny();
        assert!(is_elf32(&nso));
        let (image, tables, st) = build_module32("tiny", &nso, &file).unwrap();
        assert_eq!(&tables[..4], b"NTB2");
        assert_eq!((st.symbols, st.relocs), (3, 3));
        assert_eq!(image.len() % 0x1000, 0);
        // relocation records start after the header (152 bytes) and the symbols (24 each)
        let r = 152 + 3 * 24;
        let rec = |i: usize| {
            let o = r + 24 * i;
            (u64::from_le_bytes(tables[o..o + 8].try_into().unwrap()), i64::from_le_bytes(tables[o + 8..o + 16].try_into().unwrap()), u32::from_le_bytes(tables[o + 16..o + 20].try_into().unwrap()), u32::from_le_bytes(tables[o + 20..o + 24].try_into().unwrap()))
        };
        assert_eq!(rec(0), (0x500, 0x40, R_ARM_RELATIVE, 0));
        assert_eq!(rec(1), (0x504, 8, R_ARM_ABS32, 1));
        assert_eq!(rec(2), (0x508, 0, R_ARM_JUMP_SLOT, 2));
    }

    #[test]
    fn rejects_bad_relocation() {
        let (nso, file) = tiny();
        let mut bad = nso.clone();
        bad.text.data[0x304] = 99; // unknown relocation kind
        assert!(build_module32("bad", &bad, &file).is_err());
    }
}
