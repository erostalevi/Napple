//! Scanner for A64 sites that cannot run unmodified on macOS (rewrite targets):
//! svc, thread-pointer / counter system registers, and uses of the platform register x18.
use std::collections::BTreeMap;
use yaxpeax_arch::{Decoder, U8Reader};
use yaxpeax_arm::armv8::a64::InstDecoder;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum SiteKind {
    Svc,
    MrsTpidrEl0,
    MrsTpidrroEl0,
    MsrTpidrEl0,
    MrsCntvct,
    MrsCntpct,
    MrsCntfrq,
    OtherSysReg,
    X18,
}

#[derive(Debug, Default)]
pub struct Report {
    pub words: usize,
    pub sites: BTreeMap<SiteKind, Vec<u32>>, // module-relative offsets
}

impl Report {
    pub fn count(&self, k: SiteKind) -> usize {
        self.sites.get(&k).map_or(0, |v| v.len())
    }
}

// (op0, op1, crn, crm, op2) packed into the 15-bit id used by MRS/MSR: bits 19:5 of the word.
const fn id(op0: u32, op1: u32, crn: u32, crm: u32, op2: u32) -> u32 {
    ((op0 & 1) << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2
}
const TPIDR_EL0: u32 = id(3, 3, 13, 0, 2);
const TPIDRRO_EL0: u32 = id(3, 3, 13, 0, 3);
const CNTFRQ: u32 = id(3, 3, 14, 0, 0);
const CNTPCT: u32 = id(3, 3, 14, 0, 1);
const CNTVCT: u32 = id(3, 3, 14, 0, 2);
// System registers EL0 code may legitimately read on Apple silicon; not rewrite targets.
const ALLOWED: [u32; 5] = [id(3, 3, 4, 4, 0), id(3, 3, 4, 4, 1), id(3, 3, 0, 0, 1), id(3, 3, 0, 0, 7), id(3, 3, 4, 2, 0)];

/// Scans a code segment. `base` is only used to label offsets (module-relative).
pub fn scan(code: &[u8], base: u32) -> Report {
    let mut rep = Report::default();
    let dec = InstDecoder::default();
    for (i, w) in code.chunks_exact(4).enumerate() {
        let word = u32::from_le_bytes(w.try_into().unwrap());
        let off = base + (i as u32) * 4;
        rep.words += 1;
        if word & 0xFFE0_001F == 0xD400_0001 {
            rep.sites.entry(SiteKind::Svc).or_default().push(off);
            continue;
        }
        // MRS (read) 0xD53xxxxx, MSR register (write) 0xD51xxxxx
        if word & 0xFFF0_0000 == 0xD530_0000 || word & 0xFFF0_0000 == 0xD510_0000 {
            let read = word & 0xFFF0_0000 == 0xD530_0000;
            let s = (word >> 5) & 0x7FFF;
            let kind = match (s, read) {
                (TPIDR_EL0, true) => SiteKind::MrsTpidrEl0,
                (TPIDR_EL0, false) => SiteKind::MsrTpidrEl0,
                (TPIDRRO_EL0, true) => SiteKind::MrsTpidrroEl0,
                (CNTVCT, true) => SiteKind::MrsCntvct,
                (CNTPCT, true) => SiteKind::MrsCntpct,
                (CNTFRQ, true) => SiteKind::MrsCntfrq,
                _ if ALLOWED.contains(&s) => continue,
                _ => SiteKind::OtherSysReg,
            };
            rep.sites.entry(kind).or_default().push(off);
            continue;
        }
        // Cheap prefilter on every 5-bit register field before the (slower) full decode.
        let has18 = [word & 31, (word >> 5) & 31, (word >> 10) & 31, (word >> 16) & 31].contains(&18);
        if has18 {
            if let Ok(inst) = dec.decode(&mut U8Reader::new(w)) {
                if uses_x18(&inst.to_string()) {
                    rep.sites.entry(SiteKind::X18).or_default().push(off);
                }
            }
        }
    }
    rep
}

fn uses_x18(text: &str) -> bool {
    text.split(|c: char| !c.is_ascii_alphanumeric()).any(|t| t == "x18" || t == "w18")
}

#[cfg(test)]
mod tests {
    use super::*;

    fn w(words: &[u32]) -> Vec<u8> {
        words.iter().flat_map(|w| w.to_le_bytes()).collect()
    }

    #[test]
    fn finds_known_encodings() {
        // svc #0x1d ; mrs x0, tpidr_el0 ; msr tpidr_el0, x1 ; mrs x2, cntvct_el0 ; mrs x3, tpidrro_el0
        // mov x18, x0 (orr x18, xzr, x0) ; add x1, x2, x3 (no hit) ; mrs x4, fpcr (allowed)
        let r = scan(&w(&[0xD400_03A1, 0xD53B_D040, 0xD51B_D041, 0xD53B_E042, 0xD53B_D063, 0xAA00_03F2, 0x8B03_0041, 0xD53B_4404]), 0);
        assert_eq!(r.count(SiteKind::Svc), 1);
        assert_eq!(r.count(SiteKind::MrsTpidrEl0), 1);
        assert_eq!(r.count(SiteKind::MsrTpidrEl0), 1);
        assert_eq!(r.count(SiteKind::MrsCntvct), 1);
        assert_eq!(r.count(SiteKind::MrsTpidrroEl0), 1);
        assert_eq!(r.count(SiteKind::X18), 1);
        assert_eq!(r.count(SiteKind::OtherSysReg), 0);
    }
}
