//! Structural verifier for rewriter output. It re-reads the finished image (no emulation) and
//! checks, independently of the emitting code, that
//!
//! * nothing outside the code segment changed and every changed word is a branch into the patch area;
//! * no `x18` / `tpidr_el0` / `svc` / counter register remains in the patch area, and every use of
//!   `tpidrro_el0` is the key-page lookup sequence;
//! * every original site is covered (rewritten, inside a window, or reported unhandled);
//! * each rewritten block has the exact shape of its template, substitutes the right instruction,
//!   returns to the right address, and host gates line up with `gates.tsv`;
//! * `br x18` landing pads reach exactly `base + 4*i` and exclusive windows only exit to the
//!   original branch targets.
//!
//! Semantics (that the block computes the same thing) are checked by the Unicorn harness in `tests/`.
use crate::a64;
use crate::image::{ModuleImage, KEY_PAGE_SIZE};
use crate::scan::{self, SiteKind};
use crate::types::{Gate, GateKind, Key, RewriteOutput, SiteRow};
use std::collections::{BTreeMap, BTreeSet, HashMap};

const SPILL: i64 = -144;

#[derive(Debug, Default)]
pub struct CheckReport {
    pub violations: Vec<String>,
    /// Number of blocks verified per template.
    pub verified: BTreeMap<&'static str, usize>,
}

impl CheckReport {
    pub fn ok(&self) -> bool {
        self.violations.is_empty()
    }
}

struct Ck<'a> {
    orig: &'a ModuleImage,
    out: &'a RewriteOutput,
    rep: CheckReport,
    patch_lo: u32,
    patch_hi: u32,
}

impl Ck<'_> {
    fn bad(&mut self, addr: u32, msg: impl Into<String>) {
        if self.rep.violations.len() < 200 {
            self.rep.violations.push(format!("{addr:#x}: {}", msg.into()));
        }
    }
    fn ow(&self, a: u32) -> u32 {
        self.orig.word(a)
    }
    fn pw(&self, a: u32) -> u32 {
        let o = a as usize;
        self.out.image.get(o..o + 4).map_or(0, |b| u32::from_le_bytes(b.try_into().unwrap()))
    }
    fn in_patch(&self, a: u32) -> bool {
        a >= self.patch_lo && a < self.patch_hi
    }
    /// Target of an unconditional `b` word.
    fn b_target(&self, a: u32) -> Option<u32> {
        let w = self.pw(a);
        a64::is_uncond_b(w).then(|| a64::direct_target(w, a as u64).unwrap() as u32)
    }
}

/// Decodes the 4-word key lookup at `a`: `(t, s, key index)` for `mrs t; adrp s; ldr s,[s,#idx]; add t,t,s`.
fn key_quad(c: &Ck, a: u32) -> Option<(u32, u32, u32)> {
    let (m, p, l, d) = (c.pw(a), c.pw(a + 4), c.pw(a + 8), c.pw(a + 12));
    if m & 0xFFFF_FFE0 != 0xD53B_D060 {
        return None;
    }
    let t = m & 31;
    if p & 0x9F00_0000 != 0x9000_0000 || a64::adr_target(p, (a + 4) as u64) != Some(c.out.layout.key_page_off as u64) {
        return None;
    }
    let s = p & 31;
    let idx = (l >> 10) & 0xFFF;
    if l & 0xFFC0_0000 != 0xF940_0000 || (l >> 5) & 31 != s || l & 31 != s || idx >= 8 {
        return None;
    }
    if d != 0x8B00_0000 | (s << 16) | (t << 5) | t {
        return None;
    }
    Some((t, s, idx))
}

fn replace18(text: &str, s: u32) -> String {
    // same token rule as the rewriter: x18/w18 as whole tokens
    let mut out = String::new();
    let b = text.as_bytes();
    let mut i = 0;
    while i < b.len() {
        let boundary = i == 0 || !b[i - 1].is_ascii_alphanumeric();
        if boundary && (b[i] == b'x' || b[i] == b'w') && text[i + 1..].starts_with("18") && !text[i + 3..].starts_with(|c: char| c.is_ascii_alphanumeric()) {
            out.push(b[i] as char);
            out.push_str(&s.to_string());
            i += 3;
        } else {
            out.push(b[i] as char);
            i += 1;
        }
    }
    out
}

pub fn verify(orig: &ModuleImage, out: &RewriteOutput) -> CheckReport {
    let l = out.layout;
    let mut c = Ck { orig, out, rep: CheckReport::default(), patch_lo: l.patch_off, patch_hi: out.image.len() as u32 };
    c.layout();
    c.text_diff();
    c.coverage();
    c.patch_scan();
    c.blocks();
    c.rep
}

/// Panics with the violations (for tests).
pub fn assert_structure(orig: &ModuleImage, out: &RewriteOutput) {
    let r = verify(orig, out);
    assert!(r.ok(), "structural violations:\n{}", r.violations.join("\n"));
}

impl Ck<'_> {
    fn layout(&mut self) {
        let l = self.out.layout;
        if self.out.image.len() < l.patch_off as usize || (self.out.image.len() - l.patch_off as usize) % 4 != 0 {
            self.bad(0, "image shorter than the patch offset or patch not word aligned");
            return;
        }
        let keep_end = l.key_page_off as usize + KEY_PAGE_SIZE;
        let (o, p) = (&self.orig.bytes, &self.out.image);
        if o.len() != keep_end {
            self.bad(0, "original image length does not match the key page end");
        }
        if p[l.text_end as usize..keep_end.min(p.len())] != o[l.text_end as usize..keep_end.min(o.len())] {
            self.bad(l.text_end, "data/key page bytes differ from the original");
        }
        if p[keep_end..l.patch_off as usize].iter().any(|b| *b != 0) {
            self.bad(keep_end as u32, "padding between key page and patch area is not zero");
        }
        if p[l.key_page_off as usize..keep_end].iter().any(|b| *b != 0) {
            self.bad(l.key_page_off, "key page is not zero in the file");
        }
    }

    fn text_diff(&mut self) {
        let heads: BTreeSet<u32> = self
            .out
            .sites
            .iter()
            .filter_map(|r| match r {
                SiteRow::Single { addr, .. } | SiteRow::Call { addr, .. } => Some(*addr),
                SiteRow::Window { lo, .. } => Some(*lo),
                SiteRow::Jump { .. } => None,
            })
            .collect();
        let mut changed = BTreeSet::new();
        for a in (0..self.out.layout.text_end).step_by(4) {
            if self.ow(a) != self.pw(a) {
                changed.insert(a);
                match self.b_target(a) {
                    Some(t) if self.in_patch(t) => {}
                    _ => self.bad(a, "changed word is not a branch into the patch area"),
                }
            }
        }
        for a in changed.difference(&heads) {
            self.bad(*a, "text changed but no site row covers it");
        }
        for a in heads.difference(&changed) {
            self.bad(*a, "site row without a patched head");
        }
    }

    fn windows(&self) -> Vec<(u32, u32)> {
        self.out.sites.iter().filter_map(|r| if let SiteRow::Window { lo, hi } = r { Some((*lo, *hi)) } else { None }).collect()
    }

    fn coverage(&mut self) {
        let wins = self.windows();
        let in_win = |a: u32| wins.iter().any(|(l, h)| (*l..*h).contains(&a));
        let unhandled: BTreeSet<u32> = self.out.unhandled.iter().map(|u| u.addr).collect();
        let unhandled_win: Vec<(u32, u32)> = Vec::new();
        let _ = unhandled_win;
        let s1 = scan::scan(self.orig);
        let heads: BTreeSet<u32> = self
            .out
            .sites
            .iter()
            .filter_map(|r| match r {
                SiteRow::Single { addr, .. } | SiteRow::Call { addr, .. } => Some(*addr),
                _ => None,
            })
            .collect();
        for (a, k) in &s1.sites {
            let covered = heads.contains(a) || in_win(*a) || unhandled.contains(a) || self.out.unhandled.iter().any(|u| u.reason.starts_with("exclusive window") && (u.addr..u.addr + 4 * 20).contains(a));
            if !covered {
                self.bad(*a, format!("original {} site is not rewritten", k.label()));
            }
        }
        // rescan the finished text: whatever is still there must be dead (window body), unhandled or data
        let patched = ModuleImage { bytes: self.out.image[..self.orig.bytes.len()].to_vec(), layout: self.out.layout, mod0: self.orig.mod0.clone() };
        let s2 = scan::scan(&patched);
        for (a, k) in &s2.sites {
            let head_of_window = wins.iter().any(|(l, _)| l == a);
            if in_win(*a) && !head_of_window || unhandled.contains(a) || self.out.unhandled.iter().any(|u| u.reason.starts_with("exclusive window") && (u.addr..u.addr + 4 * 20).contains(a)) {
                continue;
            }
            self.bad(*a, format!("{} still present in rewritten text", k.label()));
        }
        // sites must have one of the expected kinds per row label
        let mut by_label: HashMap<u32, &'static str> = HashMap::new();
        for r in &self.out.sites {
            if let SiteRow::Single { addr, label } = r {
                by_label.insert(*addr, label);
            }
        }
        for (a, label) in by_label {
            match s1.sites.get(&a) {
                Some(k) if k.label() == label => {}
                other => self.bad(a, format!("row label {label} disagrees with the original scan ({other:?})")),
            }
        }
    }

    fn patch_scan(&mut self) {
        let mut a = self.patch_lo;
        while a < self.patch_hi {
            let w = self.pw(a);
            if w & 0xFFE0_001F == 0xD400_0001 {
                self.bad(a, "svc in patch area");
            }
            if w & 0xFFF0_0000 == 0xD530_0000 || w & 0xFFF0_0000 == 0xD510_0000 {
                let id = (w >> 5) & 0x7FFF;
                if w & 0xFFFF_FFE0 == 0xD53B_D060 {
                    if key_quad(self, a).is_none() {
                        self.bad(a, "mrs tpidrro_el0 is not followed by the key-page lookup");
                    }
                } else if matches!(id, 0x5E82 | 0x5E83 | 0x5F00 | 0x5F01 | 0x5F02) {
                    self.bad(a, "tpidr/counter system register in patch area");
                }
            }
            if a64::uses_reg18(w) == Some(true) {
                self.bad(a, "x18 in patch area");
            }
            a += 4;
        }
    }

    fn blocks(&mut self) {
        let gates: HashMap<u32, Gate> = self.out.gates.iter().map(|g| (g.orig, *g)).collect();
        let jumps: HashMap<u32, (u32, u32)> = self.out.sites.iter().filter_map(|r| if let SiteRow::Jump { addr, base, count } = r { Some((*addr, (*base, *count))) } else { None }).collect();
        let mut used_gates = BTreeSet::new();
        let rows = self.out.sites.clone();
        let mut pad_runs: BTreeMap<(u32, u32), u32> = BTreeMap::new();
        for r in &rows {
            match r {
                SiteRow::Single { addr, label } => {
                    let Some(entry) = self.b_target(*addr) else { continue };
                    if let Some(g) = gates.get(addr) {
                        used_gates.insert(*addr);
                        self.gate_block(*addr, entry, g);
                        continue;
                    }
                    match *label {
                        "x18" => {
                            if let Some((base, count)) = jumps.get(addr) {
                                self.br_block(*addr, entry, *base, *count, &mut pad_runs);
                            } else {
                                self.x18_block(*addr, entry);
                            }
                        }
                        "tpidr_r" => self.sysread_block(*addr, entry, Key::Ktls),
                        "tpidrro_r" => self.sysread_block(*addr, entry, Key::Ktlr),
                        "tpidr_w" => self.syswrite_block(*addr, entry),
                        _ => self.bad(*addr, format!("row {label} has no gate entry")),
                    }
                }
                SiteRow::Call { addr, .. } => {
                    if let Some(entry) = self.b_target(*addr) {
                        self.call_block(*addr, entry);
                    }
                }
                _ => {}
            }
        }
        for g in &self.out.gates {
            if !used_gates.contains(&g.orig) {
                self.bad(g.orig, "gates.tsv row without a rewritten site");
            }
        }
        self.window_blocks();
    }

    fn count(&mut self, k: &'static str) {
        *self.rep.verified.entry(k).or_insert(0) += 1;
    }

    /// `[sp, #off]` for `stp/ldp S,T,[sp,#off]`; returns `(s, t, off)`.
    fn sp_pair(&self, a: u32, load: bool) -> Option<(u32, u32, i64)> {
        let w = self.pw(a);
        let base = if load { 0xA940_0000 } else { 0xA900_0000 };
        if w & 0xFFC0_0000 != base || (w >> 5) & 31 != 31 {
            return None;
        }
        let imm = (((w >> 15) & 0x7F) as i32) << 25 >> 25;
        Some((w & 31, (w >> 10) & 31, imm as i64 * 8))
    }

    fn x18_block(&mut self, a: u32, e: u32) {
        let w = self.ow(a);
        let Some(text) = a64::disasm(w) else { return self.bad(a, "undecodable original") };
        // class by the original word
        if a64::is_cond_branch(w) {
            return self.cond_block(a, e);
        }
        if a64::is_adr_like(w) && w & 31 == 18 {
            return self.pcrel_block(a, e);
        }
        if a64::is_literal_load(w) && w & 31 == 18 {
            return self.literal_block(a, e);
        }
        let Some((s, t, off)) = self.sp_pair(e, false) else { return self.bad(a, "plain block does not start with stp S,T,[sp,#off]") };
        let Some((qt, qs, idx)) = key_quad(self, e + 4) else { return self.bad(a, "plain block lacks the key lookup") };
        if (qt, qs, idx) != (t, s, Key::K18.index()) {
            return self.bad(a, format!("plain key lookup uses wrong registers/key: {qt} {qs} {idx}"));
        }
        if self.pw(e + 20) != 0xF940_0000 | (t << 5) | s {
            self.bad(a, "plain block does not load S from the slot");
        }
        let want = replace18(&text.full, s);
        match a64::disasm(self.pw(e + 24)) {
            Some(g) if g.full == want => {}
            g => self.bad(a, format!("substituted instruction `{:?}` != `{want}`", g.map(|x| x.full))),
        }
        if self.pw(e + 28) != 0xF900_0000 | (t << 5) | s {
            self.bad(a, "plain block does not write S back to the slot");
        }
        let dlt = sp_delta_of(&text.full);
        let busy = a64::busy_regs(w);
        if busy >> s & 1 == 1 || busy >> t & 1 == 1 || s == t || s > 15 || t > 15 {
            self.bad(a, "scratch register collides with the instruction's registers");
        }
        match self.sp_pair(e + 32, true) {
            Some((ls, lt, loff)) if ls == s && lt == t && loff == off - dlt => {}
            _ => self.bad(a, "plain restore does not mirror the spill"),
        }
        if off > SPILL || (dlt < -128 && off > dlt - 16) {
            self.bad(a, format!("spill offset {off} intrudes on the red zone or the instruction's own stack writes"));
        }
        if self.b_target(e + 36) != Some(a + 4) {
            self.bad(a, "plain block does not return to the next instruction");
        }
        self.count("plain");
    }

    fn cond_block(&mut self, a: u32, e: u32) {
        let w = self.ow(a);
        let Some((s, t, off)) = self.sp_pair(e, false) else { return self.bad(a, "cond: no spill") };
        if key_quad(self, e + 4) != Some((t, s, 0)) || off != SPILL || self.pw(e + 20) != 0xF940_0000 | (t << 5) | s {
            return self.bad(a, "cond: bad lookup");
        }
        let cw = self.pw(e + 24);
        let cpc = e + 24;
        let m = if w & 0x7E00_0000 == 0x3600_0000 { 0xFFF8_0000 } else { 0xFF00_0000 };
        if cw & m != w & m || cw & 31 != s || a64::direct_target(cw, cpc as u64) != Some(cpc as u64 + 12) {
            self.bad(a, "cond: rewritten branch wrong");
        }
        let target = a64::direct_target(w, a as u64).unwrap() as u32;
        let (r1, r2) = (self.sp_pair(e + 28, true), self.sp_pair(e + 36, true));
        if r1.map(|x| (x.0, x.1, x.2)) != Some((s, t, SPILL)) || r2.map(|x| (x.0, x.1, x.2)) != Some((s, t, SPILL)) {
            self.bad(a, "cond: restore wrong");
        }
        if self.b_target(e + 32) != Some(a + 4) || self.b_target(e + 40) != Some(target) {
            self.bad(a, "cond: exits wrong");
        }
        self.count("cond");
    }

    /// Value formed by `adrp S,page ; [add S,S,#lo]` ending at `at` (the instruction after that is returned too).
    fn materialize(&self, at: u32, s: u32) -> Option<(u64, u32)> {
        let p = self.pw(at);
        if p & 0x9F00_0000 != 0x9000_0000 || p & 31 != s {
            return None;
        }
        let mut v = a64::adr_target(p, at as u64)?;
        let n = self.pw(at + 4);
        if n & 0xFFC0_03FF == 0x9100_0000 | (s << 5) | s {
            v += ((n >> 10) & 0xFFF) as u64;
            return Some((v, at + 8));
        }
        Some((v, at + 4))
    }

    fn pcrel_block(&mut self, a: u32, e: u32) {
        let w = self.ow(a);
        let expect = a64::adr_target(w, a as u64).unwrap();
        let Some((s, t, off)) = self.sp_pair(e, false) else { return self.bad(a, "pcrel: no spill") };
        if key_quad(self, e + 4) != Some((t, s, 0)) || off != SPILL {
            return self.bad(a, "pcrel: bad lookup");
        }
        let Some((v, next)) = self.materialize(e + 20, s) else { return self.bad(a, "pcrel: value not formed by adrp/add") };
        if v != expect {
            self.bad(a, format!("pcrel value {v:#x} != {expect:#x}"));
        }
        if self.pw(next) != 0xF900_0000 | (t << 5) | s || self.sp_pair(next + 4, true).map(|x| (x.0, x.1, x.2)) != Some((s, t, SPILL)) || self.b_target(next + 8) != Some(a + 4) {
            self.bad(a, "pcrel: epilogue wrong");
        }
        self.count("pcrel");
    }

    fn literal_block(&mut self, a: u32, e: u32) {
        let w = self.ow(a);
        let expect = a64::literal_target(w, a as u64).unwrap();
        let Some((s, t, off)) = self.sp_pair(e, false) else { return self.bad(a, "literal: no spill") };
        if key_quad(self, e + 4) != Some((t, s, 0)) || off != SPILL {
            return self.bad(a, "literal: bad lookup");
        }
        let Some((v, next)) = self.materialize(e + 20, s) else { return self.bad(a, "literal: address not formed") };
        let ld = [0xB940_0000u32, 0xF940_0000, 0xB980_0000][(w >> 30) as usize] | (s << 5) | s;
        if v != expect || self.pw(next) != ld || self.pw(next + 4) != 0xF900_0000 | (t << 5) | s || self.b_target(next + 12) != Some(a + 4) {
            self.bad(a, "literal: wrong address/load/epilogue");
        }
        self.count("literal");
    }

    fn sysread_block(&mut self, a: u32, e: u32, key: Key) {
        let n = self.ow(a) & 31;
        let w0 = self.pw(e);
        let tmp = w0 & 31;
        // stur tmp,[sp,#-144]
        if w0 != 0xF800_0000 | ((SPILL as u32 & 0x1FF) << 12) | (31 << 5) | tmp || ![16, 17].contains(&tmp) || tmp == n {
            return self.bad(a, "sysread: bad spill");
        }
        if key_quad(self, e + 4) != Some((n, tmp, key.index())) {
            return self.bad(a, "sysread: wrong key lookup");
        }
        if self.pw(e + 20) != 0xF940_0000 | (n << 5) | n || self.pw(e + 24) != 0xF840_0000 | ((SPILL as u32 & 0x1FF) << 12) | (31 << 5) | tmp || self.b_target(e + 28) != Some(a + 4) {
            self.bad(a, "sysread: load/restore/return wrong");
        }
        self.count("sysread");
    }

    fn syswrite_block(&mut self, a: u32, e: u32) {
        let n = self.ow(a) & 31;
        let (w0, w1) = (self.pw(e), self.pw(e + 4));
        let (t, tmp) = (w0 & 31, w1 & 31);
        let spill = |r: u32, d: i64| 0xF800_0000 | ((d as u32 & 0x1FF) << 12) | (31 << 5) | r;
        let fill = |r: u32, d: i64| 0xF840_0000 | ((d as u32 & 0x1FF) << 12) | (31 << 5) | r;
        if w0 != spill(t, SPILL) || w1 != spill(tmp, SPILL - 8) || t == n || tmp == n || tmp == t || ![16, 17].contains(&tmp) {
            return self.bad(a, "syswrite: bad spill");
        }
        if key_quad(self, e + 8) != Some((t, tmp, Key::Ktls.index())) || self.pw(e + 24) != 0xF900_0000 | (t << 5) | n || self.pw(e + 28) != fill(tmp, SPILL - 8) || self.pw(e + 32) != fill(t, SPILL) || self.b_target(e + 36) != Some(a + 4) {
            self.bad(a, "syswrite: wrong sequence");
        }
        self.count("syswrite");
    }

    fn gate_block(&mut self, a: u32, e: u32, g: &Gate) {
        let w = self.ow(a);
        let (kind, arg) = if w & 0xFFE0_001F == 0xD400_0001 {
            (GateKind::Svc, (w >> 5) & 0xFFFF)
        } else {
            match (w >> 5) & 0x7FFF {
                0x5F01 => (GateKind::Cntpct, w & 31),
                0x5F02 => (GateKind::Cntvct, w & 31),
                0x5F00 => (GateKind::Cntfrq, w & 31),
                _ => return self.bad(a, "gate on an instruction that is not svc/counter"),
            }
        };
        if g.kind != kind || g.arg != arg || g.orig != a {
            self.bad(a, "gates.tsv row does not describe the original instruction");
        }
        let words = [
            0xD100_0000 | (144 << 10) | (31 << 5) | 31,            // sub sp, sp, #144
            0xA980_0000 | (0x7E << 15) | (30 << 10) | (31 << 5) | 29, // stp x29, x30, [sp, #-16]!
        ];
        if self.pw(e) != words[0] || self.pw(e + 4) != words[1] {
            return self.bad(a, "gate: prologue wrong");
        }
        if key_quad(self, e + 8) != Some((30, 29, Key::Kgate.index())) {
            return self.bad(a, "gate: not loading KGATE");
        }
        let tail = [0xF940_0000 | (30 << 5) | 30, 0xF940_0000 | (31 << 5) | 29, a64::blr(30), 0xA8C0_0000 | (2 << 15) | (30 << 10) | (31 << 5) | 29, 0x9100_0000 | (144 << 10) | (31 << 5) | 31];
        for (i, t) in tail.iter().enumerate() {
            if self.pw(e + 24 + 4 * i as u32) != *t {
                return self.bad(a, format!("gate: word {} wrong", 6 + i));
            }
        }
        if g.ret != e + 24 + 12 {
            self.bad(a, "gate: return point does not follow the blr");
        }
        if self.b_target(e + 44) != Some(a + 4) {
            self.bad(a, "gate: does not continue after the site");
        }
        self.count("gate");
    }

    fn call_block(&mut self, a: u32, e: u32) {
        if self.ow(a) != a64::BLR_X18 {
            return self.bad(a, "call row on something that is not blr x18");
        }
        if key_quad(self, e) != Some((16, 17, Key::K18.index())) || self.pw(e + 16) != 0xF940_0000 | (16 << 5) | 16 {
            return self.bad(a, "call: slot lookup wrong");
        }
        let Some((v, next)) = self.materialize(e + 20, 30) else { return self.bad(a, "call: x30 not formed") };
        if v != (a + 4) as u64 || self.pw(next) != a64::br(16) {
            self.bad(a, "call: link register or branch wrong");
        }
        self.count("call");
    }

    fn br_block(&mut self, a: u32, e: u32, base: u32, count: u32, runs: &mut BTreeMap<(u32, u32), u32>) {
        let spill = |r: u32, d: i64| 0xF800_0000 | ((d as u32 & 0x1FF) << 12) | (31 << 5) | r;
        if self.ow(a) != a64::BR_X18 || self.pw(e) != spill(16, SPILL) || self.pw(e + 4) != spill(17, SPILL - 8) {
            return self.bad(a, "br: prologue wrong");
        }
        if key_quad(self, e + 8) != Some((16, 17, Key::K18.index())) || self.pw(e + 24) != 0xF940_0000 | (16 << 5) | 16 || self.pw(e + 28) != 0xF840_0000 | ((((SPILL - 8) as u32) & 0x1FF) << 12) | (31 << 5) | 17 {
            return self.bad(a, "br: lookup wrong");
        }
        // x16 += chain until br x16
        let mut p = e + 32;
        let mut delta: i64 = 0;
        loop {
            let w = self.pw(p);
            if w == a64::br(16) {
                break;
            }
            let (neg, sh) = match w & 0xFFC0_0000 {
                0x9100_0000 => (1, 0),
                0x9140_0000 => (1, 12),
                0xD100_0000 => (-1, 0),
                0xD140_0000 => (-1, 12),
                _ => return self.bad(a, "br: unexpected word in dispatch chain"),
            };
            if w & 0x3FF != (16 << 5) | 16 {
                return self.bad(a, "br: chain not on x16");
            }
            delta += neg * (((w >> 10) & 0xFFF) as i64) << sh;
            p += 4;
            if p > e + 32 + 4 * 14 {
                return self.bad(a, "br: dispatch chain too long");
            }
        }
        let pad_base = (base as i64 + delta) as u32;
        // the dispatcher's padding up to the pad run must be nops (first user) or the run must exist
        let key = (base, count);
        match runs.get(&key) {
            Some(prev) if *prev != pad_base => return self.bad(a, "br: pad base differs between users of the same table"),
            Some(_) => {}
            None => {
                runs.insert(key, pad_base);
                let tails = pad_base + 4 * count;
                for i in 0..count {
                    if self.b_target(pad_base + 4 * i) != Some(tails + 8 * i) {
                        return self.bad(a, format!("br: pad {i} does not reach its tail"));
                    }
                    if self.pw(tails + 8 * i) != 0xF840_0000 | ((SPILL as u32 & 0x1FF) << 12) | (31 << 5) | 16 || self.b_target(tails + 8 * i + 4) != Some(base + 4 * i) {
                        return self.bad(a, format!("br: tail {i} does not restore x16 and branch to base+4*{i}"));
                    }
                }
            }
        }
        if !self.in_patch(pad_base) || pad_base % 4 != 0 {
            self.bad(a, "br: pad base outside the patch area");
        }
        self.count("br");
    }

    /// Windows: entries, copied bodies (with x18 replaced), internal edges and exit stubs.
    fn window_blocks(&mut self) {
        let wins = self.windows();
        let mut entry: HashMap<u32, u32> = HashMap::new();
        for (lo, _) in &wins {
            if let Some(e) = self.b_target(*lo) {
                entry.insert(*lo, e);
            }
        }
        // copy start of each segment, from the entry block's final branch
        let mut copy: HashMap<u32, u32> = HashMap::new();
        let mut regs: HashMap<u32, (u32, u32)> = HashMap::new();
        for (lo, _) in &wins {
            let Some(&e) = entry.get(lo) else { continue };
            let Some((s, t, off)) = self.sp_pair(e, false) else { self.bad(*lo, "window: entry has no spill"); continue };
            if key_quad(self, e + 4) != Some((t, s, 0)) || off != SPILL || self.pw(e + 20) != 0xF940_0000 | (t << 5) | s {
                self.bad(*lo, "window: entry lookup wrong");
                continue;
            }
            match self.b_target(e + 24) {
                Some(c) => {
                    copy.insert(*lo, c);
                    regs.insert(*lo, (s, t));
                }
                None => self.bad(*lo, "window: entry does not branch to the copy"),
            }
        }
        let seg_of = |a: u32| wins.iter().find(|(l, h)| (*l..*h).contains(&a)).copied();
        for (lo, hi) in &wins {
            let (Some(&c), Some(&(s, t))) = (copy.get(lo), regs.get(lo)) else { continue };
            let sites: BTreeSet<u32> = (*lo..*hi).step_by(4).filter(|a| a64::uses_reg18(self.ow(*a)) == Some(true)).collect();
            let mut busy = 0u32;
            for a in (*lo..*hi).step_by(4) {
                busy |= a64::busy_regs(self.ow(a));
            }
            if busy >> s & 1 == 1 || busy >> t & 1 == 1 {
                self.bad(*lo, "window: scratch register collides with window registers");
            }
            let n = (*hi - *lo) / 4;
            for k in 0..n {
                let (oa, pa) = (*lo + 4 * k, c + 4 * k);
                let ow = self.ow(oa);
                let pw = self.pw(pa);
                let expected_plain = if sites.contains(&oa) { a64::subst_reg18(ow, s) } else { Some(ow) };
                if let Some(tgt) = a64::direct_target(ow, oa as u64) {
                    // same opcode/registers, displacement may differ
                    let ew = expected_plain.unwrap_or(ow);
                    let mask = if a64::is_uncond_b(ow) { 0xFC00_0000 } else if ow & 0x7E00_0000 == 0x3600_0000 { 0xFFF8_001F } else if ow & 0xFF00_0010 == 0x5400_0000 { 0xFF00_001F } else { 0xFF00_001F };
                    if pw & mask != ew & mask {
                        self.bad(oa, "window: copied branch changed beyond its displacement");
                    }
                    let dest = a64::direct_target(pw, pa as u64).unwrap() as u32;
                    if let Some((sl, _)) = seg_of(tgt as u32) {
                        // internal edge: must land on the copy of the same instruction
                        let (Some(&cl),) = (copy.get(&sl),) else { continue };
                        if dest != cl + (tgt as u32 - sl) {
                            self.bad(oa, "window: internal branch does not reach its copy");
                        }
                    } else {
                        // external: via an exit stub (str; ldp; b tgt)
                        let ok = self.pw(dest) == 0xF900_0000 | (t << 5) | s && self.sp_pair(dest + 4, true).map(|x| (x.0, x.1, x.2)) == Some((s, t, SPILL)) && self.b_target(dest + 8) == Some(tgt as u32);
                        if !ok {
                            self.bad(oa, "window: external branch does not leave through a correct exit stub");
                        }
                    }
                } else if Some(pw) != expected_plain {
                    self.bad(oa, "window: copied instruction differs from the original (modulo x18)");
                }
            }
            // fall-through exit
            let fall = self.b_target(c + 4 * n);
            match fall {
                Some(d) if seg_of(*hi).is_some_and(|(sl, _)| sl == *hi) && copy.get(hi) == Some(&d) => {}
                Some(d) => {
                    let ok = self.pw(d) == 0xF900_0000 | (t << 5) | s && self.sp_pair(d + 4, true).map(|x| (x.0, x.1, x.2)) == Some((s, t, SPILL)) && self.b_target(d + 8) == Some(*hi);
                    if !ok {
                        self.bad(*lo, "window: fall-through exit wrong");
                    }
                }
                None => self.bad(*lo, "window: missing fall-through branch"),
            }
            self.count("window");
        }
    }
}

fn sp_delta_of(full: &str) -> i64 {
    let imm = |s: &str| -> i64 {
        let s = s.trim().trim_start_matches('#');
        let (neg, s) = s.strip_prefix('-').map_or((false, s), |r| (true, r));
        let v = s.strip_prefix("0x").map_or_else(|| s.parse::<i64>().unwrap_or(0), |h| i64::from_str_radix(h, 16).unwrap_or(0));
        if neg { -v } else { v }
    };
    if let Some(p) = full.find("[sp, #") {
        let rest = &full[p + 5..];
        if let Some(end) = rest.find("]!") {
            return imm(&rest[..end]);
        }
    }
    if let Some(p) = full.find("[sp],") {
        return imm(&full[p + 5..]);
    }
    0
}

#[allow(dead_code)]
fn _kinds(_: SiteKind) {}
