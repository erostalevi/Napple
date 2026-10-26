//! Exclusive (`ldxr`..`stxr`) windows that contain x18.
//!
//! The spill/lookup prologue must run *before* the load-exclusive and the write-back *after* the
//! store-exclusive, so a window is copied whole into the patch area with x18 replaced by a scratch
//! register that holds the slot value. Branches that leave the window go through exit stubs that
//! write the scratch register back to the slot first. Out-of-line retry fragments that jump back
//! into a window are linked into one group so the loop stays inside a single copy.
use crate::a64;
use crate::image::ModuleImage;
use crate::patch::{R, Reason};
use crate::rewrite::Rewriter;
use crate::scan::SiteKind;
use crate::templates::SPILL;
use crate::types::Key;
use std::collections::{BTreeMap, BTreeSet, HashMap};

/// A straight-line run of original instructions `[lo, hi)`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Seg {
    pub lo: u32,
    pub words: Vec<u32>,
}

impl Seg {
    pub fn hi(&self) -> u32 {
        self.lo + 4 * self.words.len() as u32
    }
    pub fn contains(&self, a: u32) -> bool {
        (self.lo..self.hi()).contains(&a)
    }
    fn insns(&self) -> impl Iterator<Item = (u32, u32)> + '_ {
        self.words
            .iter()
            .enumerate()
            .map(|(i, w)| (self.lo + 4 * i as u32, *w))
    }
}

fn is_cbz_cbnz(w: u32) -> bool {
    w & 0x7E00_0000 == 0x3400_0000
}

/// Finds every load-exclusive .. store-exclusive window (plus a following `cbz/cbnz`) that holds an x18 site.
pub fn find(img: &ModuleImage, sites: &BTreeMap<u32, SiteKind>, max_len: usize) -> Vec<Seg> {
    let n = img.layout.text_end / 4;
    let mut out = Vec::new();
    let mut i = 0;
    while i < n {
        let w = img.word(i * 4);
        if !a64::is_excl_load(w) {
            i += 1;
            continue;
        }
        let mut j = i + 1;
        while j < n && !(a64::is_excl_store(img.word(j * 4)) || a64::is_excl_load(img.word(j * 4)))
        {
            j += 1;
        }
        if j < n && (j - i) < max_len as u32 && a64::is_excl_store(img.word(j * 4)) {
            if j + 1 < n && is_cbz_cbnz(img.word((j + 1) * 4)) {
                j += 1;
            }
            let (lo, hi) = (i * 4, (j + 1) * 4);
            if sites.range(lo..hi).next().is_some() {
                out.push(Seg {
                    lo,
                    words: (i..=j).map(|k| img.word(k * 4)).collect(),
                });
            }
        }
        i += 1;
    }
    out
}

/// Windows grouped with the retry fragments that re-enter them.
pub struct Grouped {
    pub groups: Vec<Vec<Seg>>,
    /// Windows that could not be handled, with the reason.
    pub rejected: Vec<(Seg, String)>,
}

/// A fragment of a retry loop laid out of line: no stores, calls or branches other than its last
/// instruction (the conditional branch back into the window).
fn retry_body_ok(frag: &[u32]) -> bool {
    frag[..frag.len() - 1].iter().all(|&x| {
        !a64::is_excl_store(x)
            && !a64::is_bl(x)
            && !a64::is_uncond_b(x)
            && !a64::is_indirect_branch(x)
            && !a64::is_cond_branch(x)
            && a64::disasm(x).is_some_and(|t| !t.mnemonic.starts_with("st"))
    })
}

/// Looks for branches from outside a window into its middle and links out-of-line retry fragments.
pub fn group(img: &ModuleImage, windows: Vec<Seg>) -> Grouped {
    let n = img.layout.text_end / 4;
    let by_lo: BTreeMap<u32, usize> = windows.iter().enumerate().map(|(k, w)| (w.lo, k)).collect();
    let owner_of = |t: u32| {
        by_lo
            .range(..=t)
            .next_back()
            .map(|(_, &k)| k)
            .filter(|&k| windows[k].contains(t))
    };
    let mut frags: Vec<Vec<Seg>> = vec![Vec::new(); windows.len()];
    let mut frag_owner: HashMap<u32, usize> = HashMap::new();
    let mut rejected: HashMap<usize, String> = HashMap::new();
    for i in 0..n {
        let (a, w) = (i * 4, img.word(i * 4));
        let Some(t) = a64::direct_target(w, a as u64).map(|t| t as u32) else {
            continue;
        };
        let Some(o) = owner_of(t) else { continue };
        if t == windows[o].lo || windows[o].contains(a) {
            continue; // a normal entry at the head, or an edge internal to the window
        }
        let mut reject = |why: String| {
            rejected.entry(o).or_insert(why);
        };
        if !a64::is_cond_branch(w) {
            reject(format!("branch at {a:#x} enters the middle of the window"));
            continue;
        }
        let floor = i.saturating_sub(15);
        let mut k = i;
        while k > floor && !a64::is_excl_load(img.word(k * 4)) {
            k -= 1;
        }
        if !a64::is_excl_load(img.word(k * 4)) {
            reject(format!(
                "branch at {a:#x} enters the window but its retry fragment has no load-exclusive"
            ));
            continue;
        }
        let frag: Vec<u32> = (k..=i).map(|x| img.word(x * 4)).collect();
        if !retry_body_ok(&frag) {
            reject(format!("unsafe exclusive retry fragment ending at {a:#x}"));
            continue;
        }
        let (lo, hi) = (k * 4, (i + 1) * 4);
        if windows.iter().any(|w2| w2.lo < hi && lo < w2.hi()) {
            reject(format!("retry fragment {lo:#x} overlaps a window"));
            continue;
        }
        match frag_owner.get(&lo) {
            Some(&prev) if prev != o => {
                reject(format!("retry fragment {lo:#x} re-enters two windows"));
            }
            Some(_) => {}
            None => {
                frag_owner.insert(lo, o);
                frags[o].push(Seg { lo, words: frag });
            }
        }
    }
    let mut groups = Vec::new();
    let mut bad = Vec::new();
    for (o, w) in windows.iter().enumerate() {
        match rejected.get(&o) {
            Some(why) => bad.push((w.clone(), why.clone())),
            None => {
                let mut g = vec![w.clone()];
                g.extend(frags[o].iter().cloned());
                groups.push(g);
            }
        }
    }
    Grouped {
        groups,
        rejected: bad,
    }
}

impl Rewriter<'_> {
    /// Emits one group of exclusive windows; returns `(window head, entry block)` pairs.
    pub(crate) fn site_window_group(&mut self, group: &[Seg]) -> R<Vec<(u32, u32)>> {
        let mut busy = 0u32;
        for s in group {
            for (a, w) in s.insns() {
                busy |= a64::busy_regs(w);
                if matches!(self.sites.get(&a), Some(k) if *k != SiteKind::X18) {
                    return Err(Reason(format!(
                        "non-x18 site at {a:#x} inside an exclusive window"
                    )));
                }
            }
        }
        let regs = a64::pick_scratch(busy, 2).ok_or("no free scratch registers for window")?;
        let (s, t) = (regs[0], regs[1]);
        const ENTRY_WORDS: u32 = 7;
        let first = self.patch.here();
        let copy_start = first + 4 * ENTRY_WORDS * group.len() as u32;
        let mut copied: HashMap<u32, u32> = HashMap::new();
        let mut cursor = copy_start;
        for seg in group {
            for (a, _) in seg.insns() {
                copied.insert(a, cursor);
                cursor += 4;
            }
            cursor += 4; // explicit fall-through branch after each fragment
        }
        // exits: fall-through of each fragment, then every external branch target
        let mut exits: Vec<u32> = Vec::new();
        let mut add_exit = |t: u32, copied: &HashMap<u32, u32>| {
            if !copied.contains_key(&t) && !exits.contains(&t) {
                exits.push(t);
            }
        };
        for seg in group {
            add_exit(seg.hi(), &copied);
            for (a, w) in seg.insns() {
                if let Some(t) = a64::direct_target(w, a as u64) {
                    add_exit(t as u32, &copied);
                }
            }
        }
        let exit_at: HashMap<u32, u32> = exits
            .iter()
            .enumerate()
            .map(|(i, t)| (*t, cursor + 16 * i as u32))
            .collect();

        let mut heads = Vec::new();
        for seg in group {
            let entry = self.patch.here();
            let p = &mut self.patch;
            p.emit(a64::stp(s, t, 31, SPILL)?);
            p.slot_addr(t, s, Key::K18)?;
            p.emit(a64::ldr_slot(s, t, 0)?);
            p.emit_b(copied[&seg.lo])?;
            heads.push((seg.lo, entry));
        }
        debug_assert_eq!(self.patch.here(), copy_start);
        for seg in group {
            for (a, w) in seg.insns() {
                let here = self.patch.here();
                debug_assert_eq!(here, copied[&a]);
                let nw = if matches!(self.sites.get(&a), Some(SiteKind::X18)) {
                    a64::subst_reg18(w, s)
                        .ok_or_else(|| Reason(format!("cannot substitute x18 at {a:#x}")))?
                } else {
                    w
                };
                // `bl` matches direct_target too, but a call cannot leave through an exit stub: it would return into
                // the copy with the scratch registers already restored
                if a64::is_bl(w) {
                    return Err(Reason(format!(
                        "instruction at {a:#x} cannot be relocated into a window"
                    )));
                }
                let out = if w & 0x9F00_0000 == 0x9000_0000 {
                    // adrp: same target page from the copy's address (x18 already replaced by the scratch register in `nw`)
                    let page = a64::adr_target(w, a as u64).ok_or("adrp target")?;
                    a64::adrp(nw & 31, here as u64, page)?
                } else if let Some(tgt) = a64::direct_target(w, a as u64) {
                    let dest = copied
                        .get(&(tgt as u32))
                        .or_else(|| exit_at.get(&(tgt as u32)))
                        .copied()
                        .ok_or("window exit missing")?;
                    a64::retarget(nw, here as u64, dest as u64)?
                } else {
                    if a64::is_indirect_branch(w)
                        || a64::is_adr_like(w)
                        || a64::is_literal_load(w)
                        || w & 0xFFE0_001F == 0xD400_0001
                    {
                        return Err(Reason(format!(
                            "instruction at {a:#x} cannot be relocated into a window"
                        )));
                    }
                    nw
                };
                self.patch.emit(out);
            }
            let hi = seg.hi();
            let dest = copied
                .get(&hi)
                .or_else(|| exit_at.get(&hi))
                .copied()
                .ok_or("window fallthrough missing")?;
            self.patch.emit_b(dest)?;
        }
        for tgt in &exits {
            debug_assert_eq!(self.patch.here(), exit_at[tgt]);
            let p = &mut self.patch;
            p.emit(a64::str_slot(s, t, 0)?);
            p.emit(a64::ldp(s, t, 31, SPILL)?);
            p.emit_b(*tgt)?;
            p.emit(a64::NOP);
        }
        Ok(heads)
    }
}

/// Addresses covered by a set of segments.
pub fn covered(groups: &[Vec<Seg>]) -> BTreeSet<u32> {
    groups
        .iter()
        .flatten()
        .flat_map(|s| (s.lo..s.hi()).step_by(4))
        .collect()
}
