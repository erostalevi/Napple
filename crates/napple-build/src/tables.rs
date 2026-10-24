//! NTB1 module tables (format in docs/runtime-modules.md), a byte-exact port of `tools/prepare_tables.py`.
//! Input: the rewriter's patched image + layout + gate rows and the original NSO (for the build id and, for
//! modules without DT_HASH, the dynsym extent). Every read is bounds-checked: modules are untrusted input.
use crate::error::{BuildError, Result};
use napple_rewrite::{Gate, GateKind};
use std::collections::BTreeMap;

const KINDS: [u32; 4] = [257, 1025, 1026, 1027]; // R_AARCH64_ABS64, GLOB_DAT, JUMP_SLOT, RELATIVE
const DT_NULL: i64 = 0;

/// Layout values the rewriter writes into `<module>.fixups.tsv`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Layout {
    pub text_end: u64,
    pub ro_off: u64,
    pub data_off: u64,
    pub patch_off: u64,
    pub key_page_off: u64,
}

/// One row of `gates.tsv`: `ret`, original instruction offset, timer (0 svc, 1 cntpct/cntvct, 2 cntfrq), arg.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GateRow {
    pub ret: u64,
    pub original: u64,
    pub timer: u32,
    pub arg: u32,
}

impl From<&Gate> for GateRow {
    fn from(g: &Gate) -> Self {
        let timer = match g.kind {
            GateKind::Svc => 0,
            GateKind::Cntfrq => 2,
            GateKind::Cntpct | GateKind::Cntvct => 1,
        };
        Self { ret: g.ret as u64, original: g.orig as u64, timer, arg: g.arg }
    }
}

fn err(name: &str, m: impl std::fmt::Display) -> BuildError {
    BuildError::new("tables", format!("{name}: {m}"))
}

fn rd<const N: usize>(b: &[u8], o: usize) -> Option<[u8; N]> {
    b.get(o..o.checked_add(N)?)?.try_into().ok()
}
fn u32_at(b: &[u8], o: usize) -> Option<u32> {
    rd::<4>(b, o).map(u32::from_le_bytes)
}
fn u64_at(b: &[u8], o: usize) -> Option<u64> {
    rd::<8>(b, o).map(u64::from_le_bytes)
}

/// Dynamic tags (first occurrence wins, like the reference tool).
fn dynamic(name: &str, image: &[u8]) -> Result<BTreeMap<i64, u64>> {
    let mod0 = u32_at(image, 4).ok_or_else(|| err(name, "image too small"))? as usize;
    if image.get(mod0..mod0 + 4) != Some(b"MOD0") {
        return Err(err(name, "MOD0 missing"));
    }
    let rel = u32_at(image, mod0 + 4).ok_or_else(|| err(name, "MOD0 truncated"))? as i32;
    let dyn_off = (mod0 as i64 + rel as i64) as usize;
    let mut tags = BTreeMap::new();
    for i in 0..4096usize {
        let (Some(t), Some(v)) = (u64_at(image, dyn_off + i * 16), u64_at(image, dyn_off + i * 16 + 8)) else {
            return Err(err(name, "dynamic table outside the image"));
        };
        if t as i64 == DT_NULL {
            return Ok(tags);
        }
        tags.entry(t as i64).or_insert(v);
    }
    Err(err(name, "unterminated dynamic table"))
}

/// DT_RELR expansion to (where, implicit addend) pairs.
fn decode_relr(name: &str, image: &[u8], tags: &BTreeMap<i64, u64>, lower: u64, upper: u64) -> Result<Vec<(u64, u64)>> {
    if ![35, 36, 37].iter().any(|k| tags.contains_key(k)) {
        return Ok(vec![]);
    }
    let (Some(&size), Some(&table), Some(&ent)) = (tags.get(&35), tags.get(&36), tags.get(&37)) else {
        return Err(err(name, "invalid ELF64 RELR dynamic tags"));
    };
    if ent != 8 || size % 8 != 0 {
        return Err(err(name, "invalid ELF64 RELR dynamic tags"));
    }
    if table.checked_add(size).is_none_or(|e| e > image.len() as u64) {
        return Err(err(name, "RELR table outside image"));
    }
    let mut out = Vec::new();
    let mut seen = std::collections::HashSet::new();
    let mut cursor: Option<u64> = None;
    let mut append = |address: u64| -> Result<()> {
        if address % 8 != 0 || address < lower || address > upper.wrapping_sub(8) || address + 8 > image.len() as u64 {
            return Err(err(name, format!("RELR relocation outside writable image: {address:#x}")));
        }
        if !seen.insert(address) {
            return Err(err(name, format!("duplicate RELR relocation: {address:#x}")));
        }
        let addend = u64_at(image, address as usize).unwrap();
        if addend >= upper {
            return Err(err(name, format!("RELR addend outside module: {address:#x}")));
        }
        out.push((address, addend));
        Ok(())
    };
    let mut off = table;
    while off < table + size {
        let entry = u64_at(image, off as usize).unwrap();
        if entry & 1 == 0 {
            append(entry)?;
            cursor = Some(entry + 8);
        } else {
            let c = cursor.ok_or_else(|| err(name, "RELR bitmap before initial address"))?;
            for bit in 1..64u64 {
                if entry & (1 << bit) != 0 {
                    append(c + (bit - 1) * 8)?;
                }
            }
            cursor = Some(c + 63 * 8);
        }
        off += 8;
    }
    Ok(out)
}

pub struct TableInput<'a> {
    pub name: &'a str,
    /// `<name>.patched.bin` as produced by the rewriter.
    pub image: &'a [u8],
    pub layout: Layout,
    pub gates: Vec<GateRow>,
    /// The original NSO file (build id at 0x40; dynsym extent at 0x98/0x9C when the module has no DT_HASH).
    pub nso: Option<&'a [u8]>,
}

/// Counts for the manifest.
pub struct TableStats {
    pub symbols: usize,
    pub relocs: usize,
    pub gates: usize,
    pub build_id: [u8; 32],
}

pub fn build_tables(inp: &TableInput) -> Result<(Vec<u8>, TableStats)> {
    let (name, image, l) = (inp.name, inp.image, inp.layout);
    let tags = dynamic(name, image)?;
    let tag = |t: i64| tags.get(&t).copied();
    let nsym = if let Some(h) = tag(4) {
        u32_at(image, h as usize + 4).ok_or_else(|| err(name, "DT_HASH outside the image"))? as usize
    } else {
        let nso = inp.nso.ok_or_else(|| err(name, "GNU-hash module needs the original NSO"))?;
        let (off, size) = (u32_at(nso, 0x98), u32_at(nso, 0x9C));
        let (Some(off), Some(size)) = (off, size) else { return Err(err(name, "NSO header truncated")) };
        if Some(l.ro_off + off as u64) != tag(6) || size % 24 != 0 {
            return Err(err(name, "dynsym extent disagrees with the NSO header"));
        }
        (size / 24) as usize
    };
    if tag(11) != Some(24) || tag(9).is_some_and(|v| v != 24) || tag(20).is_some_and(|v| v != 7) {
        return Err(err(name, "unsupported dynamic entry sizes (expected RELA with 24-byte entries)"));
    }
    let (symtab, strtab_addr, strsz) = (
        tag(6).ok_or_else(|| err(name, "DT_SYMTAB missing"))? as usize,
        tag(5).ok_or_else(|| err(name, "DT_STRTAB missing"))? as usize,
        tag(10).ok_or_else(|| err(name, "DT_STRSZ missing"))? as usize,
    );
    if nsym > (1 << 24) {
        return Err(err(name, "implausible symbol count"));
    }
    let mut strtab = vec![0u8];
    let mut offsets: std::collections::HashMap<Vec<u8>, u32> = std::collections::HashMap::new();
    let mut syms = Vec::with_capacity(nsym);
    for i in 0..nsym {
        let e = symtab + 24 * i;
        let (Some(no), Some([info, _other]), Some(shndx), Some(value), Some(size)) =
            (u32_at(image, e), rd::<2>(image, e + 4), rd::<2>(image, e + 6).map(u16::from_le_bytes), u64_at(image, e + 8), u64_at(image, e + 16))
        else {
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
        syms.push((off, info >> 4, info & 15, (shndx != 0) as u8, value, size));
    }
    let (key_off, data_off) = (l.key_page_off, l.data_off);
    let mut rels: Vec<(u64, i64, u32, u32)> = decode_relr(name, image, &tags, data_off, key_off)?.into_iter().map(|(w, a)| (w, a as i64, 1027, 0)).collect();
    let mut seen: std::collections::HashSet<u64> = rels.iter().map(|r| r.0).collect();
    for (addr, size) in [(tag(7), tag(8).unwrap_or(0)), (tag(23), tag(2).unwrap_or(0))] {
        let Some(addr) = addr.filter(|a| *a != 0) else { continue };
        if size % 24 != 0 {
            return Err(err(name, "relocation table size is not a multiple of 24"));
        }
        for off in (addr..addr + size).step_by(24) {
            let (Some(wh), Some(info), Some(addend)) = (u64_at(image, off as usize), u64_at(image, off as usize + 8), u64_at(image, off as usize + 16)) else {
                return Err(err(name, "relocation table outside the image"));
            };
            let (kind, si) = (info as u32, (info >> 32) as u32);
            if !KINDS.contains(&kind) || si as usize >= nsym {
                return Err(err(name, format!("relocation kind {kind} symbol {si}")));
            }
            if wh < data_off || wh > key_off.wrapping_sub(8) {
                return Err(err(name, "relocation outside data"));
            }
            if !seen.insert(wh) {
                return Err(err(name, "duplicate relocation"));
            }
            rels.push((wh, addend as i64, kind, si));
        }
    }
    let build_id: [u8; 32] = inp.nso.and_then(|n| rd::<32>(n, 0x40)).unwrap_or([0; 32]);
    let mut out = Vec::with_capacity(0x100 + syms.len() * 24 + rels.len() * 24 + inp.gates.len() * 24 + strtab.len());
    out.extend(b"NTB1");
    for v in [1u32, syms.len() as u32, rels.len() as u32, inp.gates.len() as u32, strtab.len() as u32] {
        out.extend(v.to_le_bytes());
    }
    for v in [image.len() as u64, l.text_end, l.ro_off, data_off, l.patch_off, key_off] {
        out.extend(v.to_le_bytes());
    }
    for t in [12, 13, 25, 27, 26, 28] {
        out.extend(tag(t).unwrap_or(0).to_le_bytes());
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
    for g in &inp.gates {
        out.extend(g.ret.to_le_bytes());
        out.extend(g.original.to_le_bytes());
        out.extend(g.timer.to_le_bytes());
        out.extend(g.arg.to_le_bytes());
    }
    out.extend(&strtab);
    Ok((out, TableStats { symbols: syms.len(), relocs: rels.len(), gates: inp.gates.len(), build_id }))
}

/// Parses the rewriter's `fixups.tsv` header lines (`# KEY<TAB>value`).
pub fn parse_fixups(text: &str) -> Result<Layout> {
    let mut m = BTreeMap::new();
    for line in text.lines() {
        if let Some((k, v)) = line.strip_prefix("# ").and_then(|l| l.split_once('\t')) {
            let v = v.trim();
            let n = match v.strip_prefix("0x") {
                Some(h) => u64::from_str_radix(h, 16).ok(),
                None => v.parse().ok(),
            };
            if let Some(n) = n {
                m.insert(k.to_string(), n);
            }
        }
    }
    let g = |k: &str| m.get(k).copied().ok_or_else(|| BuildError::new("tables", format!("fixups.tsv: {k} missing")));
    Ok(Layout { text_end: g("TEXT_END")?, ro_off: g("RO_OFF")?, data_off: g("DATA_OFF")?, patch_off: g("PATCH_OFF")?, key_page_off: g("KEY_PAGE_OFF")? })
}

/// Parses `gates.tsv` (`ret<TAB>kind<TAB>arg<TAB>orig`).
pub fn parse_gates(text: &str) -> Result<Vec<GateRow>> {
    let num = |s: &str| match s.strip_prefix("0x") {
        Some(h) => u64::from_str_radix(h, 16).ok(),
        None => s.parse().ok(),
    };
    let mut v = Vec::new();
    for line in text.lines().filter(|l| !l.trim().is_empty()) {
        let f: Vec<&str> = line.split_whitespace().collect();
        let bad = || BuildError::new("tables", format!("gates.tsv: bad row {line:?}"));
        if f.len() != 4 {
            return Err(bad());
        }
        let timer = match f[1] {
            "svc" => 0,
            "cntfrq" => 2,
            "cntpct" | "cntvct" => 1,
            _ => return Err(bad()),
        };
        v.push(GateRow { ret: num(f[0]).ok_or_else(bad)?, original: num(f[3]).ok_or_else(bad)?, timer, arg: num(f[2]).ok_or_else(bad)? as u32 });
    }
    Ok(v)
}

#[cfg(test)]
pub(crate) mod fixture {
    //! A small synthetic module (MOD0, DT_HASH, 3 symbols, RELA + JMPREL + RELR) used by the golden test.
    use super::*;

    pub const DATA_OFF: u64 = 0x2000;
    pub const KEY_OFF: u64 = 0x4000;

    pub fn image() -> Vec<u8> {
        let mut img = vec![0u8; 0x4000 + 0x4000 + 0x10000];
        let put64 = |img: &mut Vec<u8>, o: usize, v: u64| img[o..o + 8].copy_from_slice(&v.to_le_bytes());
        let put32 = |img: &mut Vec<u8>, o: usize, v: u32| img[o..o + 4].copy_from_slice(&v.to_le_bytes());
        img[..4].copy_from_slice(&0x14000000u32.to_le_bytes()); // b .+0x40000000 (irrelevant)
        put32(&mut img, 4, 0x80); // MOD0 offset
        img[0x80..0x84].copy_from_slice(b"MOD0");
        put32(&mut img, 0x84, 0x1000 - 0x80); // dynamic at 0x1000 (rodata)
        // dynamic (0x1000)
        let dyn_tags: [(i64, u64); 20] = [
            (4, 0x1200), (5, 0x1300), (6, 0x1180), (10, 0x40), (11, 24), (7, 0x2000), (8, 48), (9, 24),
            (23, 0x2040), (2, 24), (20, 7), (12, 0x10), (13, 0x20), (25, 0x2100), (27, 16), (26, 0x2110), (28, 8),
            (36, 0x1400), (35, 16), (37, 8),
        ];
        for (i, (t, v)) in dyn_tags.iter().enumerate() {
            put64(&mut img, 0x1000 + i * 16, *t as u64);
            put64(&mut img, 0x1000 + i * 16 + 8, *v);
        }
        // symtab (0x1180): null, "foo" defined function, "bar" undefined object
        let sym = |img: &mut Vec<u8>, i: usize, name: u32, info: u8, shndx: u16, v: u64, s: u64| {
            let o = 0x1180 + i * 24;
            img[o..o + 4].copy_from_slice(&name.to_le_bytes());
            img[o + 4] = info;
            img[o + 6..o + 8].copy_from_slice(&shndx.to_le_bytes());
            img[o + 8..o + 16].copy_from_slice(&v.to_le_bytes());
            img[o + 16..o + 24].copy_from_slice(&s.to_le_bytes());
        };
        sym(&mut img, 1, 1, 0x12, 1, 0x40, 8);
        sym(&mut img, 2, 5, 0x11, 0, 0, 0);
        put32(&mut img, 0x1204, 3); // DT_HASH nchain = 3
        img[0x1300..0x130A].copy_from_slice(b"\0foo\0bar\0\0");
        // RELR (0x1400): address 0x2100, then bitmap with bits for +8, +24
        put64(&mut img, 0x1400, 0x2100);
        put64(&mut img, 0x1408, 0b1010 | 1);
        put64(&mut img, 0x2100, 0x40);
        put64(&mut img, 0x2108, 0x80);
        put64(&mut img, 0x2118, 0x90);
        // RELA (0x2000): GLOB_DAT sym2 at 0x2200 ; ABS64 sym1 +4 at 0x2208
        let rela = |img: &mut Vec<u8>, o: usize, w: u64, kind: u32, si: u32, add: i64| {
            put64(img, o, w);
            put64(img, o + 8, (si as u64) << 32 | kind as u64);
            put64(img, o + 16, add as u64);
        };
        rela(&mut img, 0x2000, 0x2200, 1025, 2, 0);
        rela(&mut img, 0x2018, 0x2208, 257, 1, 4);
        rela(&mut img, 0x2040, 0x2210, 1026, 2, 0);
        img
    }

    pub fn nso() -> Vec<u8> {
        let mut n = vec![0u8; 0x100];
        n[..4].copy_from_slice(b"NSO0");
        for (i, b) in n[0x40..0x60].iter_mut().enumerate() {
            *b = i as u8 + 1;
        }
        n
    }

    pub fn layout() -> Layout {
        Layout { text_end: 0x1000, ro_off: 0x1000, data_off: DATA_OFF, patch_off: 0x8000, key_page_off: KEY_OFF }
    }

    pub fn gates() -> Vec<GateRow> {
        vec![GateRow { ret: 0x44, original: 0x40, timer: 0, arg: 5 }, GateRow { ret: 0x84, original: 0x80, timer: 1, arg: 3 }, GateRow { ret: 0xC4, original: 0xC0, timer: 2, arg: 7 }]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn input<'a>(img: &'a [u8], nso: &'a [u8]) -> TableInput<'a> {
        TableInput { name: "main", image: img, layout: fixture::layout(), gates: fixture::gates(), nso: Some(nso) }
    }

    /// Golden produced by `tools/prepare_tables.py` on the same synthetic module (see tests/golden/README).
    #[test]
    fn matches_python_golden() {
        let (img, nso) = (fixture::image(), fixture::nso());
        let (t, st) = build_tables(&input(&img, &nso)).unwrap();
        assert_eq!((st.symbols, st.relocs, st.gates), (3, 6, 3));
        let golden = include_bytes!("../tests/golden/synth.tables");
        assert_eq!(&t[..], &golden[..], "NTB1 bytes differ from prepare_tables.py");
    }

    #[test]
    fn dedups_strings_and_orders_relr_first() {
        let (img, nso) = (fixture::image(), fixture::nso());
        let (t, _) = build_tables(&input(&img, &nso)).unwrap();
        assert_eq!(&t[..4], b"NTB1");
        // header 24 + 12*8 + 32 = 152; the null symbol's empty name gets its own entry (offset 1, as in the Python tool), so foo is at 2
        let sym1 = 152 + 24;
        assert_eq!(u32::from_le_bytes(t[sym1..sym1 + 4].try_into().unwrap()), 2);
        let rel0 = 152 + 3 * 24;
        assert_eq!(u64::from_le_bytes(t[rel0..rel0 + 8].try_into().unwrap()), 0x2100);
        assert_eq!(u32::from_le_bytes(t[rel0 + 16..rel0 + 20].try_into().unwrap()), 1027);
    }

    #[test]
    fn rejects_hostile_modules_without_panicking() {
        let (img, nso) = (fixture::image(), fixture::nso());
        let mut bad = img.clone();
        bad[0x80] = b'X';
        assert!(build_tables(&input(&bad, &nso)).is_err()); // MOD0
        assert!(build_tables(&input(&img[..0x1000], &nso)).is_err()); // truncated
        let mut bad = img.clone();
        bad[0x2000 + 8..0x2000 + 12].copy_from_slice(&9u32.to_le_bytes()); // unknown relocation kind
        assert!(build_tables(&input(&bad, &nso)).is_err());
        let mut bad = img.clone();
        bad[0x2000..0x2008].copy_from_slice(&0x10u64.to_le_bytes()); // relocation outside data
        assert!(build_tables(&input(&bad, &nso)).is_err());
        let mut bad = img.clone();
        bad[0x1400..0x1408].copy_from_slice(&0x3FFF_FFF0u64.to_le_bytes()); // RELR outside image
        assert!(build_tables(&input(&bad, &nso)).is_err());
        let mut bad = img.clone();
        bad[0x1300 + 8..0x1300 + 9].copy_from_slice(b"z"); // no NUL inside DT_STRSZ is fine only when terminated
        bad[0x1300..0x1340].fill(b'a');
        assert!(build_tables(&input(&bad, &nso)).is_err());
    }

    /// Writes the fixture in the rewriter's file formats (to regenerate the golden with tools/prepare_tables.py).
    #[test]
    #[ignore]
    fn dump_fixture_for_golden() {
        let dir = std::path::PathBuf::from(std::env::var("NAPPLE_FIXTURE_DIR").expect("NAPPLE_FIXTURE_DIR"));
        std::fs::create_dir_all(dir.join("exefs")).unwrap();
        std::fs::write(dir.join("main.patched.bin"), fixture::image()).unwrap();
        std::fs::write(dir.join("exefs/main"), fixture::nso()).unwrap();
        std::fs::write(dir.join("main.fixups.tsv"), "# PATCH_OFF\t0x8000\n# KEY_PAGE_OFF\t0x4000\n# KEY_PAGE_SIZE\t0x4000\n# TEXT_END\t0x1000\n# RO_OFF\t0x1000\n# DATA_OFF\t0x2000\n").unwrap();
        std::fs::write(dir.join("main.gates.tsv"), "0x44\tsvc\t5\t0x40\n0x84\tcntvct\t3\t0x80\n0xc4\tcntfrq\t7\t0xc0\n").unwrap();
    }

    #[test]
    fn fixups_and_gates_parsers() {
        let l = parse_fixups("# PATCH_OFF\t0x8000\n# KEY_PAGE_OFF\t0x4000\n# KEY_PAGE_SIZE\t0x4000\n# TEXT_END\t0x1000\n# RO_OFF\t0x1000\n# DATA_OFF\t0x2000\n# KEY_K18_IDX\t0\n").unwrap();
        assert_eq!(l, fixture::layout());
        assert!(parse_fixups("# PATCH_OFF\t0x8000\n").is_err());
        let g = parse_gates("0x44\tsvc\t5\t0x40\n0x84\tcntvct\t3\t0x80\n0xc4\tcntfrq\t7\t0xc0\n").unwrap();
        assert_eq!(g, fixture::gates());
        assert!(parse_gates("0x44\tbogus\t5\t0x40\n").is_err());
    }
}
