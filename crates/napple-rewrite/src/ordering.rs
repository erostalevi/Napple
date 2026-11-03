//! Acquire/release ordering for guest exclusive pairs.
//!
//! Switch code often builds spinlocks from a plain `ldxr`/`stxr` acquire with no barrier after it, then reads the data
//! the lock protects. ARMv8 allows those reads to run before the store-exclusive completes (the `cbnz` on the status
//! orders only later stores), so a thread can read the protected data as it was before the previous owner's release. The
//! Cortex-A57 practically never does this; Apple cores do. Measured on an M5 with ACNH 1.0.0's job-claim lock
//! (`main+0x19b65d4`): about 1 in 2000-8000 claims took items another thread had already claimed, the cause of its
//! two-threads-record-one-command-buffer crash; with `ldaxr` none did (docs/archive/changes/2026-11-01-acnh-race-atomics-2026-11-01-r3.md).
//!
//! The pass gives every `ldxr`..`stxr` pair acquire/release ordering in place (bit 15, `ldaxr`/`stlxr`, same for byte,
//! halfword and pair forms). It only strengthens ordering, so it is correct for any code; it touches a load only when an
//! exclusive store follows it within `max_len` instructions, and never a word the scanner proved to be data.
use crate::a64;
use crate::image::ModuleImage;
use std::collections::HashSet;

/// Number of exclusive loads and stores given ordering.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Strengthened {
    pub loads: usize,
    pub stores: usize,
}

pub fn strengthen(img: &mut ModuleImage, data: &HashSet<u32>, max_len: usize) -> Strengthened {
    let n = img.layout.text_end / 4;
    let mut out = Strengthened::default();
    let mut i = 0;
    while i < n {
        let a = i * 4;
        let w = img.word(a);
        if !a64::is_excl_load(w) || data.contains(&a) {
            i += 1;
            continue;
        }
        // the pair's store: the next exclusive access, if it is a store close enough
        let mut j = i + 1;
        while j < n && j - i <= max_len as u32 && !a64::is_excl_load(img.word(j * 4)) && !a64::is_excl_store(img.word(j * 4)) {
            j += 1;
        }
        let s = j * 4;
        if j < n && j - i <= max_len as u32 && a64::is_excl_store(img.word(s)) && !data.contains(&s) {
            if let Some(o) = a64::ordered_exclusive(w) {
                img.set_word(a, o);
                out.loads += 1;
            }
            if let Some(o) = a64::ordered_exclusive(img.word(s)) {
                img.set_word(s, o);
                out.stores += 1;
            }
            i = j + 1;
        } else {
            i += 1;
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::scan::tests::module;

    const LDXR_W10_X8: u32 = 0x885F_7D0A; // ldxr w10, [x8]
    const STXR_W10_W9_X8: u32 = 0x880A_7D09; // stxr w10, w9, [x8]
    const CBNZ_W10_M4: u32 = 0x35FF_FFEA; // cbnz w10, .-4
    const LDP_W9_W26: u32 = 0x2958_6AA9; // ldp w9, w26, [x21, #0xc0]
    const NOP: u32 = 0xD503_201F;

    #[test]
    fn job_claim_lock_gets_acquire_release() {
        // ACNH 1.0.0 main+0x19b65e4: ldxr w10; cbnz w10 (retry); stxr w10, w9; cbnz w10; then the protected loads
        let w = [NOP, LDXR_W10_X8, CBNZ_W10_M4, STXR_W10_W9_X8, CBNZ_W10_M4, LDP_W9_W26, NOP, NOP];
        let mut img = module(&w);
        let r = strengthen(&mut img, &HashSet::new(), 16);
        assert_eq!(r, Strengthened { loads: 1, stores: 1 });
        assert_eq!(img.word(4), 0x885F_FD0A, "ldaxr w10, [x8]");
        assert_eq!(img.word(12), 0x880A_FD09, "stlxr w10, w9, [x8]");
        assert_eq!(img.word(20), LDP_W9_W26, "other words untouched");
        assert!(a64::is_ordering_upgrade(LDXR_W10_X8, img.word(4)));
    }

    #[test]
    fn already_ordered_and_pairs() {
        // ldaxr/stlxr stay as they are; ldxp/stxp get the bit too
        let ldaxr = LDXR_W10_X8 | a64::EXCL_ORDERED;
        let stlxr = STXR_W10_W9_X8 | a64::EXCL_ORDERED;
        let ldxp = 0xC87F_0C02u32; // ldxp x2, x3, [x0]
        let stxp = 0xC824_0C02u32; // stxp w4, x2, x3, [x0]
        let w = [ldaxr, stlxr, ldxp, NOP, stxp, NOP];
        let mut img = module(&w);
        let r = strengthen(&mut img, &HashSet::new(), 16);
        assert_eq!(r, Strengthened { loads: 1, stores: 1 });
        assert_eq!(img.word(0), ldaxr);
        assert_eq!(img.word(4), stlxr);
        assert_eq!(img.word(8), ldxp | a64::EXCL_ORDERED);
        assert_eq!(img.word(16), stxp | a64::EXCL_ORDERED);
    }

    #[test]
    fn lone_or_data_words_untouched() {
        // a load without a store in reach, a store without a load, and a pair the scanner proved to be data
        let mut w = vec![LDXR_W10_X8];
        w.extend(std::iter::repeat_n(NOP, 20));
        w.extend([STXR_W10_W9_X8, NOP, LDXR_W10_X8, STXR_W10_W9_X8]);
        let mut img = module(&w);
        let data: HashSet<u32> = [23 * 4].into_iter().collect();
        let r = strengthen(&mut img, &data, 16);
        assert_eq!(r, Strengthened::default());
        assert_eq!(img.word(0), LDXR_W10_X8);
        assert_eq!(img.word(21 * 4), STXR_W10_W9_X8);
        assert_eq!(img.word(23 * 4), LDXR_W10_X8);
    }
}
