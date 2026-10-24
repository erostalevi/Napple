//! Orchestration: scan, group exclusive windows, then rewrite every remaining site.
use crate::a64;
use crate::error::{RewriteError, Unhandled};
use crate::image::ModuleImage;
use crate::jumptab::{self, Shape};
use crate::patch::{Patch, Reason, R};
use crate::scan::{self, SiteKind};
use crate::types::{Gate, GateKind, Key, RewriteOptions, RewriteOutput, SiteRow, Stats};
use crate::templates::SPILL;
use crate::windows;
use napple_core::infrastructure::nso::Nso;
use std::collections::{BTreeMap, BTreeSet};

pub(crate) struct Rewriter<'a> {
    pub orig: &'a ModuleImage,
    /// Working copy of the image (text gets patched in place).
    pub img: Vec<u8>,
    pub patch: Patch,
    pub sites: BTreeMap<u32, SiteKind>,
    pub starts: Vec<u32>,
    pub opts: &'a RewriteOptions,
    pub gates: Vec<Gate>,
    pub rows: Vec<SiteRow>,
    pub stats: Stats,
    pub unhandled: Vec<Unhandled>,
    pub done: BTreeSet<u32>,
    /// `(base, count)` -> address of the landing-pad run already emitted for that table.
    pub pads: BTreeMap<(u32, u32), u32>,
    jump_rows: Vec<SiteRow>,
    window_rows: Vec<SiteRow>,
    call_rows: Vec<SiteRow>,
}

impl<'a> Rewriter<'a> {
    fn new(orig: &'a ModuleImage, sites: BTreeMap<u32, SiteKind>, starts: Vec<u32>, opts: &'a RewriteOptions) -> Self {
        let l = orig.layout;
        Self {
            orig,
            img: orig.bytes.clone(),
            patch: Patch::new(l.patch_off, l.key_page_off),
            sites,
            starts,
            opts,
            gates: Vec::new(),
            rows: Vec::new(),
            stats: Stats::new(),
            unhandled: Vec::new(),
            done: BTreeSet::new(),
            pads: BTreeMap::new(),
            jump_rows: Vec::new(),
            window_rows: Vec::new(),
            call_rows: Vec::new(),
        }
    }

    fn stat(&mut self, k: &'static str) {
        *self.stats.entry(k).or_insert(0) += 1;
    }

    fn fail(&mut self, addr: u32, reason: impl Into<String>) {
        self.unhandled.push(Unhandled { addr, word: self.orig.word(addr), reason: reason.into() });
    }

    /// Replaces the original word at `at` with a branch into the patch area.
    fn put_branch(&mut self, at: u32, to: u32) -> R<()> {
        let w = a64::b(at as u64, to as u64)?;
        self.img[at as usize..at as usize + 4].copy_from_slice(&w.to_le_bytes());
        Ok(())
    }

    /// Enclosing function `[start, end)` from the unwind table, if the address is covered.
    fn function_of(&self, a: u32) -> Option<(u32, u32)> {
        let i = self.starts.partition_point(|s| *s <= a).checked_sub(1)?;
        let end = self.starts.get(i + 1).copied().unwrap_or(self.orig.layout.text_end);
        Some((self.starts[i], end))
    }

    fn rewrite_windows(&mut self) {
        let wins = windows::find(self.orig, &self.sites, self.opts.max_window_len);
        let grouped = windows::group(self.orig, wins);
        for (seg, why) in grouped.rejected {
            self.fail(seg.lo, format!("exclusive window rejected: {why}"));
            self.done.extend((seg.lo..seg.hi()).step_by(4));
        }
        for group in grouped.groups {
            let mark = self.patch.mark();
            let addrs: Vec<u32> = group.iter().flat_map(|s| (s.lo..s.hi()).step_by(4)).collect();
            let result = self.site_window_group(&group).and_then(|heads| {
                for (head, entry) in &heads {
                    self.put_branch(*head, *entry)?;
                }
                Ok(())
            });
            match result {
                Ok(()) => {
                    for s in &group {
                        self.window_rows.push(SiteRow::Window { lo: s.lo, hi: s.hi() });
                        self.stat("window");
                    }
                }
                Err(Reason(why)) => {
                    self.patch.rollback(mark);
                    self.fail(group[0].lo, format!("exclusive window: {why}"));
                }
            }
            self.done.extend(addrs);
        }
    }

    /// An x18 instruction between a load-exclusive and its store-exclusive that no window covers
    /// would get spill code inserted into the reservation; refuse instead of risking livelock.
    fn inside_open_exclusive(&self, a: u32) -> bool {
        let floor = a.saturating_sub(4 * 24);
        let mut p = a;
        while p > floor {
            p -= 4;
            let w = self.orig.word(p);
            if a64::is_excl_store(w) {
                return false;
            }
            if a64::is_excl_load(w) {
                return true;
            }
        }
        false
    }

    fn rewrite_site(&mut self, a: u32, kind: SiteKind) -> R<(u32, &'static str)> {
        let w = self.orig.word(a);
        match kind {
            SiteKind::X18 => {
                if w == a64::BR_X18 {
                    let (jt, start) = self.site_br(a)?;
                    self.jump_rows.push(SiteRow::Jump { addr: a, base: jt.0, count: jt.1 });
                    return Ok((start, "br"));
                }
                if w == a64::BLR_X18 {
                    let start = self.site_call(a)?;
                    self.call_rows.push(SiteRow::Call { addr: a, ret: a + 4 });
                    return Ok((start, "call_window"));
                }
                if self.inside_open_exclusive(a) {
                    return Err("x18 instruction inside a ldxr/stxr sequence that no window covers".into());
                }
                if a64::is_literal_load(w) && (w & 31) == 18 {
                    return Ok((self.site_literal(a, w)?, "literal"));
                }
                if a64::is_adr_like(w) && (w & 31) == 18 {
                    return Ok((self.site_pcrel(a, w)?, "pcrel"));
                }
                if a64::is_cond_branch(w) && !a64::is_adr_like(w) && a64::direct_target(w, 0).is_some() && !a64::is_uncond_b(w) {
                    return Ok((self.site_cond(a, w)?, "cond"));
                }
                if a64::is_indirect_branch(w) || a64::is_uncond_b(w) || a64::is_bl(w) {
                    return Err(Reason(format!("unsupported branch form with x18 ({w:08x})")));
                }
                Ok((self.site_plain(a, w)?, "plain"))
            }
            SiteKind::TpidrRead(n) => Ok((self.site_sysreg_read(a, n, Key::Ktls)?, "tpidr_r")),
            SiteKind::TpidrroRead(n) => Ok((self.site_sysreg_read(a, n, Key::Ktlr)?, "tpidrro_r")),
            SiteKind::TpidrWrite(n) => Ok((self.site_tpidr_write(a, n)?, "tpidr_w")),
            SiteKind::Svc(imm) => Ok((self.site_gate(a, GateKind::Svc, imm as u32)?, "svc")),
            SiteKind::Cntpct(n) => Ok((self.site_gate(a, GateKind::Cntpct, n)?, "cntpct")),
            SiteKind::Cntvct(n) => Ok((self.site_gate(a, GateKind::Cntvct, n)?, "cntvct")),
            SiteKind::Cntfrq(n) => Ok((self.site_gate(a, GateKind::Cntfrq, n)?, "cntfrq")),
        }
    }

    /// `br x18` through a table of landing pads; returns `((base, count), block start)`.
    fn site_br(&mut self, a: u32) -> R<((u32, u32), u32)> {
        let func = self.function_of(a);
        let jt = jumptab::analyze(self.orig, a, func, self.opts.max_jump_pads).map_err(Reason)?;
        let (base, count) = (jt.base, jt.count);
        let key = (base, count);
        let p = &mut self.patch;
        let start = p.here();
        // x16/x17 can be live across a jump table; borrow them through the red zone
        p.emit(a64::stur(16, 31, SPILL)?);
        p.emit(a64::stur(17, 31, SPILL - 8)?);
        p.slot_addr(16, 17, Key::K18)?;
        p.emit(a64::ldr_slot(16, 16, 0)?); // x16 = virtual x18 = base + 4*i
        p.emit(a64::ldur(17, 31, SPILL - 8)?);
        const STUB_WORDS: u32 = 24;
        let pad_base = self.pads.get(&key).copied().unwrap_or(start + 4 * STUB_WORDS);
        let delta = pad_base as i64 - base as i64;
        let (hi, lo) = ((delta.unsigned_abs() >> 12) as u32, (delta.unsigned_abs() & 0xFFF) as u32);
        let mut ops = Vec::new();
        let mut rest = hi;
        while rest > 0 {
            let h = rest.min(0xFFF);
            ops.push(if delta >= 0 { a64::add_imm(16, 16, h, true) } else { a64::sub_imm(16, 16, h, true) });
            rest -= h;
        }
        if lo != 0 {
            ops.push(if delta >= 0 { a64::add_imm(16, 16, lo, false) } else { a64::sub_imm(16, 16, lo, false) });
        }
        if ops.len() > 12 {
            return Err("landing pads too far from the table base".into());
        }
        for o in ops {
            p.emit(o);
        }
        p.emit(a64::br(16));
        if !self.pads.contains_key(&key) {
            while self.patch.here() < pad_base {
                self.patch.emit(a64::NOP);
            }
            if self.patch.here() != pad_base {
                return Err("jump stub overflow".into());
            }
            let tails = pad_base + 4 * count;
            for i in 0..count {
                self.patch.emit_b(tails + 8 * i)?;
            }
            for i in 0..count {
                self.patch.emit(a64::ldur(16, 31, SPILL)?);
                self.patch.emit_b(base + 4 * i)?;
            }
            self.pads.insert(key, pad_base);
        }
        match jt.how {
            Shape::FunctionSpan => self.stat("jump_function_span"),
            _ => self.stat("jump_exact"),
        }
        Ok(((base, count), start))
    }

    fn run(&mut self) {
        self.rewrite_windows();
        let todo: Vec<(u32, SiteKind)> = self.sites.iter().map(|(a, k)| (*a, *k)).collect();
        for (a, kind) in todo {
            if self.done.contains(&a) {
                continue;
            }
            let mark = self.patch.mark();
            let gates = self.gates.len();
            let jrows = self.jump_rows.len();
            let crows = self.call_rows.len();
            let res = self.rewrite_site(a, kind).and_then(|(start, stat)| {
                self.put_branch(a, start)?;
                Ok(stat)
            });
            match res {
                Ok(stat) => {
                    self.stat(stat);
                    if stat != "call_window" {
                        self.rows.push(SiteRow::Single { addr: a, label: kind.label() });
                    }
                }
                Err(Reason(why)) => {
                    self.patch.rollback(mark);
                    // landing pads emitted by this site were rolled back too: forget them so no later table reuses them
                    let end = self.patch.here();
                    self.pads.retain(|_, at| *at < end);
                    self.gates.truncate(gates);
                    self.jump_rows.truncate(jrows);
                    self.call_rows.truncate(crows);
                    self.fail(a, why);
                }
            }
        }
    }
}

/// Rewrites one NSO module. `build_id` is the hex build id recorded in the outputs (the first 32
/// hex digits, as in the reference tool, identify the build).
pub fn rewrite_module(nso: &Nso, build_id: &str, opts: &RewriteOptions) -> Result<RewriteOutput, RewriteError> {
    let orig = ModuleImage::from_nso(nso)?;
    let scanned = scan::scan(&orig);
    let starts = orig.function_starts();
    let mut warnings = Vec::new();
    for (a, k) in &scanned.data_matches {
        warnings.push(format!("{a:#x}: data word in the code segment decodes as {}; left untouched", k.label()));
    }
    for a in &scanned.undecodable {
        warnings.push(format!("{a:#x}: undecodable word with a register-18 field ({:08x}); left untouched", orig.word(*a)));
    }
    for (a, id, read) in &scanned.other_sysregs {
        warnings.push(format!("{a:#x}: {} of system register {id:#x} is not rewritten", if *read { "mrs" } else { "msr" }));
    }
    let mut rw = Rewriter::new(&orig, scanned.sites, starts, opts);
    rw.run();
    let layout = orig.layout;
    let mut image = rw.img;
    image.resize(layout.patch_off as usize, 0);
    image.extend_from_slice(&rw.patch.bytes());
    let mut sites = rw.rows;
    sites.extend(rw.window_rows);
    sites.extend(rw.call_rows);
    sites.extend(rw.jump_rows);
    Ok(RewriteOutput {
        image,
        layout,
        build_id: build_id.to_string(),
        fixups: rw.patch.fixups,
        gates: rw.gates,
        sites,
        stats: rw.stats,
        unhandled: rw.unhandled,
        warnings,
    })
}
