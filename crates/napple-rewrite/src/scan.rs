//! Linear scan of the code segment for every word that cannot run unmodified on macOS.
//!
//! The scan is purely word based (no control-flow recovery): each aligned word is tested against
//! the `svc`/system-register encodings and, if it names a register-18 field, decoded to confirm
//! `x18`/`w18` is a real operand. Words that are provably data (inline literals) are excluded.
use crate::a64;
use crate::image::ModuleImage;
use rayon::prelude::*;
use std::collections::{BTreeMap, HashSet};

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum SiteKind {
    Svc(u16),
    /// `mrs xN, tpidr_el0`
    TpidrRead(u32),
    /// `msr tpidr_el0, xN`
    TpidrWrite(u32),
    /// `mrs xN, tpidrro_el0`
    TpidrroRead(u32),
    Cntpct(u32),
    Cntvct(u32),
    Cntfrq(u32),
    /// Any instruction naming `x18`/`w18`.
    X18,
}

impl SiteKind {
    /// Label used in `sites.tsv` (same vocabulary as the reference rewriter, plus `cntfrq`).
    pub fn label(self) -> &'static str {
        match self {
            SiteKind::Svc(_) => "svc",
            SiteKind::TpidrRead(_) => "tpidr_r",
            SiteKind::TpidrWrite(_) => "tpidr_w",
            SiteKind::TpidrroRead(_) => "tpidrro_r",
            SiteKind::Cntpct(_) => "cntpct",
            SiteKind::Cntvct(_) => "cntvct",
            SiteKind::Cntfrq(_) => "cntfrq",
            SiteKind::X18 => "x18",
        }
    }
}

/// 15-bit `op0[0]:op1:CRn:CRm:op2` system register identifier (bits 19:5 of MRS/MSR).
const fn sysreg(op0: u32, op1: u32, crn: u32, crm: u32, op2: u32) -> u32 {
    ((op0 & 1) << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2
}
const TPIDR_EL0: u32 = sysreg(3, 3, 13, 0, 2);
const TPIDRRO_EL0: u32 = sysreg(3, 3, 13, 0, 3);
const CNTFRQ_EL0: u32 = sysreg(3, 3, 14, 0, 0);
const CNTPCT_EL0: u32 = sysreg(3, 3, 14, 0, 1);
const CNTVCT_EL0: u32 = sysreg(3, 3, 14, 0, 2);
/// Registers EL0 code may use directly on Apple silicon: ctr_el0, dczid_el0, fpcr, fpsr, nzcv.
const NATIVE_SYSREGS: [u32; 5] = [sysreg(3, 3, 0, 0, 1), sysreg(3, 3, 0, 0, 7), sysreg(3, 3, 4, 4, 0), sysreg(3, 3, 4, 4, 1), sysreg(3, 3, 4, 2, 0)];

#[derive(Debug, Default)]
pub struct ScanResult {
    /// Module offset -> what must be rewritten there.
    pub sites: BTreeMap<u32, SiteKind>,
    /// Matches in words that are provably data (left untouched).
    pub data_matches: Vec<(u32, SiteKind)>,
    /// Words with a register-18 field the decoder could not decode (neither rewritten nor proven data).
    pub undecodable: Vec<u32>,
    /// MRS/MSR of system registers that are neither native nor rewritten: `(offset, id, is_read)`.
    pub other_sysregs: Vec<(u32, u32, bool)>,
}

fn classify(w: u32) -> Classified {
    if w & 0xFFE0_001F == 0xD400_0001 {
        return Classified::Site(SiteKind::Svc(((w >> 5) & 0xFFFF) as u16));
    }
    if w & 0xFFF0_0000 == 0xD530_0000 || w & 0xFFF0_0000 == 0xD510_0000 {
        let read = w & 0xFFF0_0000 == 0xD530_0000;
        let (id, rt) = ((w >> 5) & 0x7FFF, w & 31);
        let kind = match (id, read) {
            (TPIDR_EL0, true) => SiteKind::TpidrRead(rt),
            (TPIDR_EL0, false) => SiteKind::TpidrWrite(rt),
            (TPIDRRO_EL0, true) => SiteKind::TpidrroRead(rt),
            (CNTPCT_EL0, true) => SiteKind::Cntpct(rt),
            (CNTVCT_EL0, true) => SiteKind::Cntvct(rt),
            (CNTFRQ_EL0, true) => SiteKind::Cntfrq(rt),
            _ if NATIVE_SYSREGS.contains(&id) => return Classified::None,
            _ => return Classified::OtherSysreg(id, read),
        };
        return Classified::Site(kind);
    }
    if a64::may_use_reg18(w) {
        return match a64::uses_reg18(w) {
            Some(true) => Classified::Site(SiteKind::X18),
            Some(false) => Classified::None,
            None => Classified::Undecodable,
        };
    }
    Classified::None
}

enum Classified {
    None,
    Site(SiteKind),
    OtherSysreg(u32, bool),
    Undecodable,
}

/// Offsets of words inside the code segment that are data: the MOD0 header, the word after a
/// `bl pc+8` (the "get pc" literal idiom) and the targets of pc-relative literal loads.
fn data_words(img: &ModuleImage) -> HashSet<u32> {
    let text = img.layout.text_end;
    let mut set = HashSet::new();
    if let Some(m) = &img.mod0 {
        for o in (m.offset..m.offset + 0x24).step_by(4) {
            set.insert(o);
        }
    }
    let chunks: Vec<HashSet<u32>> = (0..text / 4)
        .collect::<Vec<u32>>()
        .par_chunks(1 << 16)
        .map(|c| {
            let mut s = HashSet::new();
            for &i in c {
                let (pc, w) = (i * 4, img.word(i * 4));
                if a64::is_bl(w) && a64::direct_target(w, pc as u64) == Some(pc as u64 + 8) {
                    s.insert(pc + 4);
                }
                if let Some(t) = a64::literal_target(w, pc as u64) {
                    let size = match (w >> 30, w >> 26 & 1) {
                        (0 | 2 | 3, 0) | (0, 1) => 4,
                        (1, _) => 8,
                        _ => 16,
                    };
                    if t < text as u64 {
                        for k in (0..size).step_by(4) {
                            s.insert(t as u32 + k);
                        }
                    }
                }
            }
            s
        })
        .collect();
    for c in chunks {
        set.extend(c);
    }
    set
}

pub fn scan(img: &ModuleImage) -> ScanResult {
    let n = (img.layout.text_end / 4) as usize;
    let parts: Vec<ScanResult> = (0..n)
        .collect::<Vec<usize>>()
        .par_chunks(1 << 16)
        .map(|c| {
            let mut r = ScanResult::default();
            for &i in c {
                let off = (i * 4) as u32;
                match classify(img.word(off)) {
                    Classified::None => {}
                    Classified::Site(k) => {
                        r.sites.insert(off, k);
                    }
                    Classified::OtherSysreg(id, read) => r.other_sysregs.push((off, id, read)),
                    Classified::Undecodable => r.undecodable.push(off),
                }
            }
            r
        })
        .collect();
    let mut out = ScanResult::default();
    for p in parts {
        out.sites.extend(p.sites);
        out.other_sysregs.extend(p.other_sysregs);
        out.undecodable.extend(p.undecodable);
    }
    let data = data_words(img);
    let moved: Vec<u32> = out.sites.keys().copied().filter(|o| data.contains(o)).collect();
    for o in moved {
        let k = out.sites.remove(&o).unwrap();
        out.data_matches.push((o, k));
    }
    out.undecodable.retain(|o| !data.contains(o));
    out.other_sysregs.retain(|(o, _, _)| !data.contains(o));
    out
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::image::ModuleImage;
    use napple_core::infrastructure::nso::{Nso, Segment};

    pub(crate) fn module(words: &[u32]) -> ModuleImage {
        let nso = Nso {
            build_id: [1; 32],
            text: Segment { mem_offset: 0, data: words.iter().flat_map(|w| w.to_le_bytes()).collect() },
            rodata: Segment { mem_offset: 0x1000, data: vec![0; 0x100] },
            data: Segment { mem_offset: 0x2000, data: vec![0; 0x100] },
            bss_size: 0x100,
        };
        ModuleImage::from_nso(&nso).unwrap()
    }

    #[test]
    fn finds_every_kind() {
        let img = module(&[
            0xD400_03A1, // svc #0x1d
            0xD53B_D040, // mrs x0, tpidr_el0
            0xD51B_D041, // msr tpidr_el0, x1
            0xD53B_D063, // mrs x3, tpidrro_el0
            0xD53B_E025, // mrs x5, cntpct_el0
            0xD53B_E046, // mrs x6, cntvct_el0
            0xD53B_E007, // mrs x7, cntfrq_el0
            0xAA00_03F2, // mov x18, x0
            0x8B03_0041, // add x1, x2, x3 (none)
            0xD53B_4404, // mrs x4, fpcr (native)
            0xD538_0000, // mrs x0, midr_el1 (other)
        ]);
        let r = scan(&img);
        let kinds: Vec<_> = r.sites.values().copied().collect();
        assert_eq!(
            kinds,
            vec![
                SiteKind::Svc(0x1d),
                SiteKind::TpidrRead(0),
                SiteKind::TpidrWrite(1),
                SiteKind::TpidrroRead(3),
                SiteKind::Cntpct(5),
                SiteKind::Cntvct(6),
                SiteKind::Cntfrq(7),
                SiteKind::X18
            ]
        );
        assert_eq!(r.other_sysregs.len(), 1);
        assert!(r.undecodable.is_empty());
    }

    #[test]
    fn inline_literals_are_not_rewritten() {
        // bl .+8 ; <data word that looks like mov x18, x0> ; ret ; ldr x1, lit ; ... ; lit: mov x18,x0
        let img = module(&[0x9400_0002, 0xAA00_03F2, 0xD65F_03C0, 0x5800_0041, 0xD503_201F, 0xAA00_03F2]);
        let r = scan(&img);
        assert!(r.sites.is_empty(), "{:?}", r.sites);
        assert_eq!(r.data_matches.len(), 2);
    }
}
