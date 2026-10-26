//! BKTR (update patch) RomFS: AES-CTR-EX subsection table + relocation table over the base RomFS.
//!
//! UNVALIDATED AGAINST RETAIL: there is no real update available, so this is implemented from the public NCA/BKTR
//! format description and checked only with synthetic fixtures (see the tests). The assumptions that retail data must
//! confirm are listed in docs/changes/2026-10-22-packaging.md.
//!
//! Model. The update's RomFS section (FS header encryption type 4) holds, in this order, the patch storage
//! (`[0, relocation_offset)`: new/changed data and the new IVFC hash levels), the relocation bucket tree and the
//! subsection ("AES-CTR-EX") bucket tree. The virtual (patched) address space is the section as a whole including the
//! IVFC levels; the relocation table maps virtual ranges either to the base RomFS section (source 0) or to the patch
//! storage (source 1). The patched RomFS is IVFC level 6 of the update's IVFC header inside that virtual space.
//! Patch-storage ranges are AES-CTR encrypted with the section key; the counter's generation word (counter bytes 4..8)
//! comes from the subsection entry covering the range, the low 8 bytes are `absolute NCA offset >> 4`. The two tables
//! themselves use the plain section counter.
use crate::error::{BuildError, Result};
use aes::cipher::{generic_array::GenericArray, BlockDecrypt, KeyInit, KeyIvInit, StreamCipher, StreamCipherSeek};
use aes::Aes128;
use napple_core::domain::error::{Error as CoreError, Result as CoreResult};
use napple_core::domain::keys::KeySet;
use napple_core::infrastructure::io::{check, map_ctr, read_vec, MapExt, Slice, Source, Src};
use napple_core::infrastructure::nca::TitleKeys;
use std::sync::Arc;
use xts_mode::Xts128;

const NODE_SIZE: u64 = 0x4000;
const NODE_HEADER: u64 = 0x10;
const MAX_ENTRIES: u64 = 1 << 22;

fn bad(m: impl std::fmt::Display) -> BuildError {
    BuildError::new("bktr", m.to_string())
}
fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn u64le(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}

// ---- minimal NCA3 section access (napple-core keeps the section key private) ------------------------------------

pub struct RawSection {
    pub index: usize,
    pub start: u64,
    pub end: u64,
    pub fs_type: u8,
    pub enc_type: u8,
    pub fs_header: Vec<u8>,
}

pub struct RawNca {
    src: Src,
    ctr_key: [u8; 16],
    pub sections: Vec<RawSection>,
}

fn ecb_decrypt(key: &[u8; 16], data: &mut [u8]) {
    let c = Aes128::new(GenericArray::from_slice(key));
    for b in data.chunks_exact_mut(16) {
        c.decrypt_block(GenericArray::from_mut_slice(b));
    }
}

impl RawNca {
    pub fn open(src: Src, keys: &KeySet, tks: &TitleKeys) -> Result<Self> {
        if src.size() < 0xC00 {
            return Err(bad("NCA too small"));
        }
        let mut h = read_vec(&*src, 0, 0xC00)?;
        let hk: [u8; 32] = keys.get("header_key")?;
        let xts = Xts128::<Aes128>::new(Aes128::new(GenericArray::from_slice(&hk[..16])), Aes128::new(GenericArray::from_slice(&hk[16..])));
        for (i, s) in h.chunks_exact_mut(0x200).enumerate() {
            xts.decrypt_sector(s, (i as u128).to_be_bytes());
        }
        if &h[0x200..0x204] != b"NCA3" {
            return Err(BuildError::new("integrity", "NCA header magic (wrong keys or unsupported NCA version)"));
        }
        let key_gen = h[0x206].max(h[0x220]).saturating_sub(1);
        let rights: [u8; 16] = h[0x230..0x240].try_into().unwrap();
        let ctr_key = if rights != [0u8; 16] {
            let mut k = *tks.get(&rights).ok_or_else(|| BuildError::new("keys", "ticket for this NCA's rights id is missing"))?;
            let tk: [u8; 16] = keys.get(&format!("titlekek_{key_gen:02x}"))?;
            ecb_decrypt(&tk, &mut k);
            k
        } else {
            let kind = match h[0x207] {
                0 => "application",
                1 => "ocean",
                2 => "system",
                _ => return Err(bad("unsupported key area index")),
            };
            let kek: [u8; 16] = keys.get(&format!("key_area_key_{kind}_{key_gen:02x}"))?;
            let mut area = [0u8; 64];
            area.copy_from_slice(&h[0x300..0x340]);
            ecb_decrypt(&kek, &mut area);
            area[0x20..0x30].try_into().unwrap()
        };
        let mut sections = Vec::new();
        for i in 0..4 {
            let e = 0x240 + i * 0x10;
            let (s, en) = (u32le(&h, e) as u64 * 0x200, u32le(&h, e + 4) as u64 * 0x200);
            if en <= s {
                continue;
            }
            check(s, en - s, src.size())?;
            let fh = h[0x400 + i * 0x200..0x400 + (i + 1) * 0x200].to_vec();
            sections.push(RawSection { index: i, start: s, end: en, fs_type: fh[2], enc_type: fh[4], fs_header: fh });
        }
        Ok(Self { src, ctr_key, sections })
    }

    /// Plain AES-CTR view of a section (what napple-core does for encryption type 3).
    pub fn ctr_section(&self, s: &RawSection) -> Result<Src> {
        Ok(Arc::new(CtrSource::new(Slice::new(self.src.clone(), s.start, s.end - s.start)?, self.ctr_key, section_ctr(&s.fs_header), s.start)))
    }

    pub fn raw_section(&self, s: &RawSection) -> Result<Src> {
        Ok(Slice::new(self.src.clone(), s.start, s.end - s.start)?)
    }
}

/// (secure value, generation) as the 8 big-endian IV bytes 0..8 (FS header 0x140: generation LE, 0x144: secure value LE).
fn section_ctr(fh: &[u8]) -> [u8; 8] {
    let mut hi: [u8; 8] = fh[0x140..0x148].try_into().unwrap();
    hi.reverse();
    hi
}

/// AES-CTR with the IV `hi || BE128(absolute_offset >> 4)`; `gen_override` replaces IV bytes 4..8 (AES-CTR-EX).
pub struct CtrSource {
    raw: Src,
    key: [u8; 16],
    hi: [u8; 8],
    base: u64,
}

impl CtrSource {
    pub fn new(raw: Src, key: [u8; 16], hi: [u8; 8], base: u64) -> Self {
        Self { raw, key, hi, base }
    }

    fn apply(&self, offset: u64, buf: &mut [u8], generation: Option<u32>) -> CoreResult<()> {
        let abs = self.base + offset;
        let pre = (abs % 16) as usize;
        let mut tmp = vec![0u8; pre + buf.len()];
        self.raw.read_at(offset - pre as u64, &mut tmp)?;
        let mut iv = [0u8; 16];
        iv[..8].copy_from_slice(&self.hi);
        if let Some(g) = generation {
            iv[4..8].copy_from_slice(&g.to_be_bytes());
        }
        let mut c = ctr::Ctr128BE::<Aes128>::new(GenericArray::from_slice(&self.key), GenericArray::from_slice(&iv));
        c.seek(abs - pre as u64);
        c.apply_keystream(&mut tmp);
        buf.copy_from_slice(&tmp[pre..]);
        Ok(())
    }

    fn map_gen(&self, offset: u64, len: u64, generation: Option<u32>, out: &mut Vec<MapExt>) -> bool {
        let mut hi = self.hi;
        if let Some(g) = generation {
            hi[4..8].copy_from_slice(&g.to_be_bytes());
        }
        check(offset, len, self.raw.size()).is_ok() && map_ctr(&*self.raw, offset, len, self.key, hi, self.base, out)
    }
}

impl Source for CtrSource {
    fn size(&self) -> u64 {
        self.raw.size()
    }
    fn read_at(&self, offset: u64, buf: &mut [u8]) -> CoreResult<()> {
        check(offset, buf.len() as u64, self.raw.size())?;
        self.apply(offset, buf, None)
    }
    fn map(&self, offset: u64, len: u64, out: &mut Vec<MapExt>) -> bool {
        self.map_gen(offset, len, None, out)
    }
}

// ---- bucket trees ---------------------------------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RelocEntry {
    pub virtual_off: u64,
    pub physical_off: u64,
    /// 0 = base RomFS section, 1 = patch storage.
    pub source: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SubEntry {
    pub offset: u64,
    pub generation: u32,
}

/// Reads the entries of a bucket tree stored in `table` (entry sets start after the L1 node and any L2 nodes).
/// Also returns the end offset recorded in the first node header.
fn bucket_entries(table: &[u8], count: u64, entry_size: u64, what: &str) -> Result<(Vec<Vec<u8>>, u64)> {
    if count == 0 || count > MAX_ENTRIES {
        return Err(bad(format!("{what}: implausible entry count {count}")));
    }
    if table.len() < NODE_SIZE as usize {
        return Err(bad(format!("{what}: table too small")));
    }
    let per_set = (NODE_SIZE - NODE_HEADER) / entry_size;
    let sets = count.div_ceil(per_set);
    let offsets_per_node = (NODE_SIZE - NODE_HEADER) / 8;
    let l2 = if sets <= offsets_per_node { 0 } else { sets.div_ceil(offsets_per_node) };
    let base = NODE_SIZE * (1 + l2);
    let end_offset = u64le(table, 8);
    let mut out = Vec::with_capacity(count as usize);
    for k in 0..sets {
        let at = base + k * NODE_SIZE;
        if at + NODE_HEADER > table.len() as u64 {
            return Err(bad(format!("{what}: entry set {k} outside the table")));
        }
        let at = at as usize;
        let n = u32le(table, at + 4) as u64;
        let expect = per_set.min(count - k * per_set);
        if n != expect || at as u64 + NODE_HEADER + n * entry_size > table.len() as u64 {
            return Err(bad(format!("{what}: entry set {k} has {n} entries, expected {expect}")));
        }
        for i in 0..n {
            let s = at + NODE_HEADER as usize + (i * entry_size) as usize;
            out.push(table[s..s + entry_size as usize].to_vec());
        }
    }
    Ok((out, end_offset))
}

pub fn parse_relocations(table: &[u8], count: u64) -> Result<(Vec<RelocEntry>, u64)> {
    let (raw, end) = bucket_entries(table, count, 0x14, "relocation table")?;
    let v: Vec<RelocEntry> = raw.iter().map(|e| RelocEntry { virtual_off: u64le(e, 0), physical_off: u64le(e, 8), source: u32le(e, 16) }).collect();
    if v[0].virtual_off != 0 || v.windows(2).any(|w| w[0].virtual_off >= w[1].virtual_off) || v.iter().any(|e| e.source > 1) {
        return Err(bad("relocation table is not sorted/starting at 0 or names an unknown source"));
    }
    Ok((v, end))
}

pub fn parse_subsections(table: &[u8], count: u64) -> Result<Vec<SubEntry>> {
    let (raw, _) = bucket_entries(table, count, 0x10, "subsection table")?;
    let v: Vec<SubEntry> = raw.iter().map(|e| SubEntry { offset: u64le(e, 0), generation: u32le(e, 12) }).collect();
    if v[0].offset != 0 || v.windows(2).any(|w| w[0].offset >= w[1].offset) {
        return Err(bad("subsection table is not sorted/starting at 0"));
    }
    Ok(v)
}

// ---- patched storage ------------------------------------------------------------------------------------------------

/// Patch storage: ciphertext decrypted with the generation of the covering subsection entry.
struct ExCtrSource {
    inner: CtrSource,
    subs: Vec<SubEntry>,
    /// First byte of the tables (plain counter beyond it).
    data_end: u64,
}

impl Source for ExCtrSource {
    fn size(&self) -> u64 {
        self.inner.size()
    }
    fn read_at(&self, offset: u64, buf: &mut [u8]) -> CoreResult<()> {
        check(offset, buf.len() as u64, self.size())?;
        let (mut pos, mut done) = (offset, 0usize);
        while done < buf.len() {
            let (gen_, limit) = if pos >= self.data_end {
                (None, self.size())
            } else {
                let i = self.subs.partition_point(|s| s.offset <= pos) - 1;
                let next = self.subs.get(i + 1).map(|s| s.offset).unwrap_or(self.data_end).min(self.data_end);
                (Some(self.subs[i].generation), next)
            };
            let n = ((limit - pos) as usize).min(buf.len() - done);
            self.inner.apply(pos, &mut buf[done..done + n], gen_)?;
            pos += n as u64;
            done += n;
        }
        Ok(())
    }
    fn map(&self, offset: u64, len: u64, out: &mut Vec<MapExt>) -> bool {
        if check(offset, len, self.size()).is_err() {
            return false;
        }
        let (mut pos, end) = (offset, offset + len);
        while pos < end {
            let (gen_, limit) = if pos >= self.data_end {
                (None, self.size())
            } else {
                let i = self.subs.partition_point(|s| s.offset <= pos) - 1;
                let next = self.subs.get(i + 1).map(|s| s.offset).unwrap_or(self.data_end).min(self.data_end);
                (Some(self.subs[i].generation), next)
            };
            let n = (limit - pos).min(end - pos);
            if !self.inner.map_gen(pos, n, gen_, out) {
                return false;
            }
            pos += n;
        }
        true
    }
}

/// Virtual address space of the patched section, composed from the base section and the patch storage.
struct IndirectSource {
    relocs: Vec<RelocEntry>,
    total: u64,
    base: Src,
    patch: Src,
}

impl Source for IndirectSource {
    fn size(&self) -> u64 {
        self.total
    }
    fn read_at(&self, offset: u64, buf: &mut [u8]) -> CoreResult<()> {
        check(offset, buf.len() as u64, self.total)?;
        let (mut pos, mut done) = (offset, 0usize);
        while done < buf.len() {
            let i = self.relocs.partition_point(|r| r.virtual_off <= pos) - 1;
            let e = self.relocs[i];
            let end = self.relocs.get(i + 1).map(|r| r.virtual_off).unwrap_or(self.total);
            let n = ((end - pos) as usize).min(buf.len() - done);
            let phys = e.physical_off + (pos - e.virtual_off);
            let src = if e.source == 0 { &self.base } else { &self.patch };
            if phys.checked_add(n as u64).is_none_or(|x| x > src.size()) {
                return Err(CoreError::Container("relocation points outside its source".into()));
            }
            src.read_at(phys, &mut buf[done..done + n])?;
            pos += n as u64;
            done += n;
        }
        Ok(())
    }
    fn map(&self, offset: u64, len: u64, out: &mut Vec<MapExt>) -> bool {
        if check(offset, len, self.total).is_err() {
            return false;
        }
        let (mut pos, end) = (offset, offset + len);
        while pos < end {
            let i = self.relocs.partition_point(|r| r.virtual_off <= pos) - 1;
            let e = self.relocs[i];
            let lim = self.relocs.get(i + 1).map(|r| r.virtual_off).unwrap_or(self.total);
            let n = (lim - pos).min(end - pos);
            let src = if e.source == 0 { &self.base } else { &self.patch };
            if !src.map(e.physical_off + (pos - e.virtual_off), n, out) {
                return false;
            }
            pos += n;
        }
        true
    }
}

/// One mapping of the patched RomFS *data level* (translated out of the section space) for `romfs.delta`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DeltaEntry {
    pub data_offset: u64,
    pub source_offset: u64,
    pub source: u32,
}

pub struct PatchedRomfs {
    /// The patched RomFS image (IVFC data level), same bytes `romfs.bin` would hold for the update.
    pub data: Src,
    pub data_off: u64,
    pub data_size: u64,
    pub relocs: Vec<RelocEntry>,
    /// Decrypted patch storage (`[0, relocation_offset)`).
    pub patch: Src,
    pub patch_size: u64,
}

/// Opens the BKTR section `sec` of an update NCA over `base` (the base RomFS section, or the base data level, see the
/// pipeline: both interpretations are tried and the one that yields a parseable RomFS wins).
pub fn open_patched(nca: &RawNca, sec: &RawSection, base: Src) -> Result<PatchedRomfs> {
    if sec.fs_type != 0 || sec.enc_type != 4 {
        return Err(bad("section is not a BKTR RomFS section"));
    }
    let fh = &sec.fs_header;
    let size = sec.end - sec.start;
    let (reloc_off, reloc_size, reloc_cnt) = (u64le(fh, 0x100), u64le(fh, 0x108), u32le(fh, 0x118) as u64);
    let (sub_off, sub_size, sub_cnt) = (u64le(fh, 0x120), u64le(fh, 0x128), u32le(fh, 0x138) as u64);
    if &fh[0x110..0x114] != b"BKTR" || &fh[0x130..0x134] != b"BKTR" {
        return Err(bad("BKTR table headers missing in the FS header"));
    }
    for (o, s) in [(reloc_off, reloc_size), (sub_off, sub_size)] {
        if o.checked_add(s).is_none_or(|e| e > size) || s > (1 << 28) {
            return Err(bad("BKTR table outside the section"));
        }
    }
    let plain = nca.ctr_section(sec)?;
    let reloc_tab = read_vec(&*plain, reloc_off, reloc_size as usize)?;
    let sub_tab = read_vec(&*plain, sub_off, sub_size as usize)?;
    let (relocs, end_offset) = parse_relocations(&reloc_tab, reloc_cnt)?;
    let subs = parse_subsections(&sub_tab, sub_cnt)?;
    if &fh[8..12] != b"IVFC" {
        return Err(bad("patched section has no IVFC header"));
    }
    let l = 0x10 + 5 * 0x18;
    let (data_off, data_size) = (u64le(fh, 8 + l), u64le(fh, 8 + l + 8));
    let total = end_offset.max(data_off.saturating_add(data_size));
    if total > (1 << 44) || data_off.checked_add(data_size).is_none_or(|e| e > total) {
        return Err(bad("patched RomFS lies outside the virtual section"));
    }
    let raw = nca.raw_section(sec)?;
    let patch_len = reloc_off.min(sub_off);
    let patch: Src = Arc::new(ExCtrSource { inner: CtrSource::new(raw, nca.ctr_key, section_ctr(fh), sec.start), subs, data_end: patch_len });
    let patch_view = Slice::new(patch.clone(), 0, patch_len)?;
    let ind: Src = Arc::new(IndirectSource { relocs: relocs.clone(), total, base, patch: patch_view.clone() });
    Ok(PatchedRomfs { data: Slice::new(ind, data_off, data_size)?, data_off, data_size, relocs, patch: patch_view, patch_size: patch_len })
}

impl PatchedRomfs {
    /// Relocation entries clipped to the data level and translated to data-level offsets. `base_data_shift` is the
    /// offset of the base RomFS data level inside the base source space used for `base` (0 when `base` was the data level).
    pub fn delta_entries(&self, base_data_shift: u64) -> Result<Vec<DeltaEntry>> {
        let (lo, hi) = (self.data_off, self.data_off + self.data_size);
        let mut out: Vec<DeltaEntry> = Vec::new();
        for (i, r) in self.relocs.iter().enumerate() {
            let end = self.relocs.get(i + 1).map(|n| n.virtual_off).unwrap_or(u64::MAX);
            let (s, e) = (r.virtual_off.max(lo), end.min(hi));
            if s >= e {
                continue;
            }
            let phys = r.physical_off + (s - r.virtual_off);
            let source_offset = if r.source == 0 {
                phys.checked_sub(base_data_shift).ok_or_else(|| bad("a relocation reads the base IVFC hash levels; the delta layer cannot express it"))?
            } else {
                phys
            };
            out.push(DeltaEntry { data_offset: s - lo, source_offset, source: r.source });
        }
        Ok(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use napple_core::infrastructure::io::BytesSource;

    pub fn tree(entries: &[Vec<u8>], entry_size: usize, end: u64) -> Vec<u8> {
        let per_set = (NODE_SIZE as usize - 0x10) / entry_size;
        let sets = entries.chunks(per_set).count();
        let mut t = vec![0u8; NODE_SIZE as usize * (1 + sets)];
        t[4..8].copy_from_slice(&(sets as u32).to_le_bytes());
        t[8..16].copy_from_slice(&end.to_le_bytes());
        for (k, chunk) in entries.chunks(per_set).enumerate() {
            let at = NODE_SIZE as usize * (1 + k);
            t[at..at + 4].copy_from_slice(&(k as u32).to_le_bytes());
            t[at + 4..at + 8].copy_from_slice(&(chunk.len() as u32).to_le_bytes());
            for (i, e) in chunk.iter().enumerate() {
                let o = at + 0x10 + i * entry_size;
                t[o..o + entry_size].copy_from_slice(e);
            }
        }
        t
    }

    fn reloc(v: u64, p: u64, s: u32) -> Vec<u8> {
        let mut e = Vec::new();
        e.extend(v.to_le_bytes());
        e.extend(p.to_le_bytes());
        e.extend(s.to_le_bytes());
        e
    }
    fn sub(o: u64, g: u32) -> Vec<u8> {
        let mut e = Vec::new();
        e.extend(o.to_le_bytes());
        e.extend(0u32.to_le_bytes());
        e.extend(g.to_le_bytes());
        e
    }

    #[test]
    fn bucket_tree_parsing_single_and_multiple_sets() {
        let few: Vec<Vec<u8>> = (0..5).map(|i| reloc(i * 0x100, i * 0x10, (i % 2) as u32)).collect();
        let (r, end) = parse_relocations(&tree(&few, 0x14, 0x500), 5).unwrap();
        assert_eq!((r.len(), end, r[3]), (5, 0x500, RelocEntry { virtual_off: 0x300, physical_off: 0x30, source: 1 }));
        // 1000 entries = two entry sets (818 per set)
        let many: Vec<Vec<u8>> = (0..1000).map(|i| sub(i * 0x10, i as u32)).collect();
        let s = parse_subsections(&tree(&many, 0x10, 0x4000), 1000).unwrap();
        assert_eq!((s.len(), s[999].generation), (1000, 999));
    }

    #[test]
    fn bucket_tree_rejects_bad_tables() {
        let t = tree(&[reloc(0, 0, 0), reloc(0x100, 0, 1)], 0x14, 0x200);
        assert!(parse_relocations(&t, 3).is_err()); // count mismatch with the entry set
        assert!(parse_relocations(&t[..100], 2).is_err());
        assert!(parse_relocations(&tree(&[reloc(0x10, 0, 0)], 0x14, 0x20), 1).is_err()); // must start at 0
        assert!(parse_relocations(&tree(&[reloc(0, 0, 0), reloc(0, 0, 0)], 0x14, 0x20), 2).is_err()); // not increasing
        assert!(parse_relocations(&tree(&[reloc(0, 0, 7)], 0x14, 0x20), 1).is_err()); // source
        assert!(parse_relocations(&t, 0).is_err() && parse_relocations(&t, u64::MAX).is_err());
        assert!(parse_subsections(&tree(&[sub(5, 0)], 0x10, 0x20), 1).is_err());
    }

    #[test]
    fn ex_ctr_uses_the_covering_generation() {
        let key = [0x33u8; 16];
        let hi = [0, 0, 0, 7, 0, 0, 0, 1];
        let plain: Vec<u8> = (0..0x400u32).map(|i| (i * 31) as u8).collect();
        let subs = vec![SubEntry { offset: 0, generation: 1 }, SubEntry { offset: 0x105, generation: 9 }, SubEntry { offset: 0x2f0, generation: 3 }];
        // encrypt range by range with the same rule
        let base = 0x2000u64;
        let mut enc = plain.clone();
        for (i, s) in subs.iter().enumerate() {
            let end = subs.get(i + 1).map(|n| n.offset).unwrap_or(0x380) as usize;
            let mut iv = [0u8; 16];
            iv[..8].copy_from_slice(&hi);
            iv[4..8].copy_from_slice(&s.generation.to_be_bytes());
            let mut c = ctr::Ctr128BE::<Aes128>::new(GenericArray::from_slice(&key), GenericArray::from_slice(&iv));
            c.seek(base + s.offset);
            c.apply_keystream(&mut enc[s.offset as usize..end]);
        }
        // tail [0x380, 0x400) uses the plain section counter
        let mut iv = [0u8; 16];
        iv[..8].copy_from_slice(&hi);
        let mut c = ctr::Ctr128BE::<Aes128>::new(GenericArray::from_slice(&key), GenericArray::from_slice(&iv));
        c.seek(base + 0x380);
        c.apply_keystream(&mut enc[0x380..]);
        let src = ExCtrSource { inner: CtrSource::new(Arc::new(BytesSource(enc)), key, hi, base), subs, data_end: 0x380 };
        for (off, len) in [(0usize, 0x400usize), (3, 0x200), (0x100, 20), (0x37e, 5), (0x2ef, 3)] {
            let mut b = vec![0u8; len.min(0x400 - off)];
            src.read_at(off as u64, &mut b).unwrap();
            assert_eq!(b, plain[off..off + b.len()], "read {off:#x}+{len:#x}");
        }
        assert!(src.read_at(0x3ff, &mut [0u8; 2]).is_err());
    }

    #[test]
    fn indirect_storage_composes_base_and_patch() {
        let base: Vec<u8> = (0..200u32).map(|i| i as u8).collect();
        let patch: Vec<u8> = (0..100u32).map(|i| (200 + i) as u8).collect();
        let relocs = vec![
            RelocEntry { virtual_off: 0, physical_off: 10, source: 0 },
            RelocEntry { virtual_off: 30, physical_off: 5, source: 1 },
            RelocEntry { virtual_off: 50, physical_off: 100, source: 0 },
        ];
        let s = IndirectSource { relocs, total: 90, base: Arc::new(BytesSource(base.clone())), patch: Arc::new(BytesSource(patch.clone())) };
        let mut all = vec![0u8; 90];
        s.read_at(0, &mut all).unwrap();
        let mut expect = base[10..40].to_vec();
        expect.extend(&patch[5..25]);
        expect.extend(&base[100..140]);
        assert_eq!(all, expect);
        let mut mid = [0u8; 10];
        s.read_at(25, &mut mid).unwrap();
        assert_eq!(&mid[..], &expect[25..35]);
        assert!(s.read_at(85, &mut [0u8; 10]).is_err());
        let bad = IndirectSource { relocs: vec![RelocEntry { virtual_off: 0, physical_off: 190, source: 0 }], total: 90, base: Arc::new(BytesSource(base)), patch: Arc::new(BytesSource(patch)) };
        assert!(bad.read_at(0, &mut [0u8; 20]).is_err());
    }
}
