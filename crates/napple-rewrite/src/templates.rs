//! Single-instruction rewrite templates. Each returns the patch-area address of the block that
//! replaces the instruction; the caller overwrites the original word with a branch to it.
//!
//! Conventions shared by all templates:
//! * Borrowed registers are spilled at `[sp, #-144]`: just below the 128-byte red zone that leaf
//!   functions of the guest may use for live locals (LLVM/AArch64 does), and restored before
//!   control returns to the original code stream. Runtimes must therefore run signal handlers
//!   for guest threads on an alternate stack (see `docs/architecture/rewrite-format.md`).
//! * Scratch registers are chosen from x0..x15 and never collide with a register the rewritten
//!   instruction names; flags (NZCV) are never touched.
//! * Virtual x18 lives in thread slot `K18`; the slot address is `tpidrro_el0 + key-page[K18]`.
use crate::a64::{self, Text};
use crate::patch::{R, Reason};
use crate::rewrite::Rewriter;
use crate::types::{Gate, GateKind, Key};

/// Spill slot offset below `sp` (16 bytes at `sp-144`, below the 128-byte red zone).
pub(crate) const SPILL: i32 = -144;

/// Parses `#-0x10`, `#0x10`, `#16`.
fn parse_imm(s: &str) -> Option<i64> {
    let s = s.trim().strip_prefix('#')?;
    let (neg, s) = match s.strip_prefix('-') {
        Some(r) => (true, r),
        None => (false, s),
    };
    let v = match s.strip_prefix("0x") {
        Some(h) => i64::from_str_radix(h, 16).ok()?,
        None => s.parse::<i64>().ok()?,
    };
    Some(if neg { -v } else { v })
}

/// Amount an instruction adds to `sp` through writeback (`[sp, #i]!` / `[sp], #i`).
fn sp_delta(t: &Text) -> R<i64> {
    let full = &t.full;
    if let Some(p) = full.find("[sp, #") {
        let rest = &full[p + 5..];
        if let Some(end) = rest.find("]!") {
            return parse_imm(&rest[..end])
                .ok_or_else(|| Reason(format!("unparsable sp writeback in `{full}`")));
        }
        // plain `[sp, #imm]`: accesses deeper than the red zone would collide with the spill slot
        if let Some(end) = rest.find(']')
            && parse_imm(&rest[..end]).is_some_and(|v| v < -128)
        {
            return Err(Reason(format!(
                "sp-relative access below the red zone in `{full}`"
            )));
        }
    }
    if let Some(p) = full.find("[sp],") {
        return parse_imm(&full[p + 5..])
            .ok_or_else(|| Reason(format!("register post-index on sp in `{full}`")));
    }
    if matches!(t.ops.first().map(String::as_str), Some("sp" | "wsp"))
        && !t.mnemonic.starts_with("st")
    {
        return Err(Reason(format!("instruction writes sp: `{full}`")));
    }
    Ok(0)
}

impl Rewriter<'_> {
    fn scratch2(&self, w: u32, extra_busy: u32) -> R<(u32, u32)> {
        let v = a64::pick_scratch(a64::busy_regs(w) | extra_busy, 2)
            .ok_or("no free scratch registers")?;
        Ok((v[0], v[1]))
    }

    /// Any instruction that reads or writes x18 as an ordinary operand.
    pub(crate) fn site_plain(&mut self, a: u32, w: u32) -> R<u32> {
        if a64::is_excl_load(w) || a64::is_excl_store(w) {
            return Err("exclusive access with x18 outside a recognised ldxr/stxr window".into());
        }
        let text = a64::disasm(w).ok_or("undecodable instruction")?;
        let dlt = sp_delta(&text)?;
        let (s, t) = self.scratch2(w, 0)?;
        let nw = a64::subst_reg18(w, s)
            .ok_or_else(|| Reason(format!("cannot substitute x18 in `{}`", text.full)))?;
        // a push writes between the new sp and the old one: keep the spill clear of that range
        let save: i32 = if dlt < -128 { dlt as i32 - 16 } else { SPILL };
        if !(-368..=368).contains(&dlt) {
            return Err(Reason(format!("sp writeback {dlt} out of range")));
        }
        let p = &mut self.patch;
        let start = p.here();
        p.emit(a64::stp(s, t, 31, save)?);
        p.slot_addr(t, s, Key::K18)?;
        p.emit(a64::ldr_slot(s, t, 0)?);
        p.emit(nw);
        // always write back: the decoder can under-report register writes (e.g. flag-setting forms)
        p.emit(a64::str_slot(s, t, 0)?);
        p.emit(a64::ldp(s, t, 31, save - dlt as i32)?);
        p.emit_b(a + 4)?;
        Ok(start)
    }

    /// `cbz/cbnz/tbz/tbnz x18, target`
    pub(crate) fn site_cond(&mut self, a: u32, w: u32) -> R<u32> {
        let target = a64::direct_target(w, a as u64).ok_or("not a conditional branch")? as u32;
        let (s, t) = self.scratch2(w, 0)?;
        let p = &mut self.patch;
        let start = p.here();
        p.emit(a64::stp(s, t, 31, SPILL)?);
        p.slot_addr(t, s, Key::K18)?;
        p.emit(a64::ldr_slot(s, t, 0)?);
        let pc = p.here();
        let taken = pc + 12;
        // Rt is bits 4:0 in all four forms; only the displacement changes
        let nw = a64::retarget((w & !31) | s, pc as u64, taken as u64)?;
        p.emit(nw);
        p.emit(a64::ldp(s, t, 31, SPILL)?);
        p.emit_b(a + 4)?;
        p.emit(a64::ldp(s, t, 31, SPILL)?);
        p.emit_b(target)?;
        Ok(start)
    }

    /// `adr/adrp x18, label`: the value is a link-time constant, so the slot just receives it.
    pub(crate) fn site_pcrel(&mut self, a: u32, w: u32) -> R<u32> {
        let target = a64::adr_target(w, a as u64).ok_or("not adr/adrp")?;
        let (s, t) = (0, 1);
        let p = &mut self.patch;
        let start = p.here();
        p.emit(a64::stp(s, t, 31, SPILL)?);
        p.slot_addr(t, s, Key::K18)?;
        let pc = p.here();
        p.emit(a64::adrp(s, pc as u64, target)?);
        if target & 0xFFF != 0 {
            p.emit(a64::add_imm(s, s, (target & 0xFFF) as u32, false));
        }
        p.emit(a64::str_slot(s, t, 0)?);
        p.emit(a64::ldp(s, t, 31, SPILL)?);
        p.emit_b(a + 4)?;
        Ok(start)
    }

    /// `ldr x18/w18/ldrsw x18, literal`: load from the literal's absolute address into the slot.
    pub(crate) fn site_literal(&mut self, a: u32, w: u32) -> R<u32> {
        let target = a64::literal_target(w, a as u64).ok_or("not a literal load")?;
        let ld = match w >> 30 {
            0 => 0xB940_0000u32, // ldr w
            1 => 0xF940_0000,    // ldr x
            2 => 0xB980_0000,    // ldrsw
            _ => return Err("unsupported literal load".into()),
        };
        let (s, t) = (0, 1);
        let p = &mut self.patch;
        let start = p.here();
        p.emit(a64::stp(s, t, 31, SPILL)?);
        p.slot_addr(t, s, Key::K18)?;
        let pc = p.here();
        p.emit(a64::adrp(s, pc as u64, target)?);
        if target & 0xFFF != 0 {
            p.emit(a64::add_imm(s, s, (target & 0xFFF) as u32, false));
        }
        p.emit(ld | (s << 5) | s);
        p.emit(a64::str_slot(s, t, 0)?);
        p.emit(a64::ldp(s, t, 31, SPILL)?);
        p.emit_b(a + 4)?;
        Ok(start)
    }

    /// `blr x18`: load virtual x18 into x16, set the link register to the original return point and
    /// branch. x16/x17 are intra-procedure-call scratch registers (dead into any call), so nothing
    /// needs spilling and the callee sees exactly the architectural state of `blr x18`.
    pub(crate) fn site_call(&mut self, a: u32) -> R<u32> {
        let ret = a + 4;
        let p = &mut self.patch;
        let start = p.here();
        p.slot_addr(16, 17, Key::K18)?;
        p.emit(a64::ldr_slot(16, 16, 0)?);
        let pc = p.here();
        p.emit(a64::adrp(30, pc as u64, ret as u64)?);
        p.emit(a64::add_imm(30, 30, ret & 0xFFF, false));
        p.emit(a64::br(16));
        Ok(start)
    }

    /// `mrs xN, tpidr_el0` / `mrs xN, tpidrro_el0` -> xN = thread slot.
    pub(crate) fn site_sysreg_read(&mut self, a: u32, n: u32, key: Key) -> R<u32> {
        if n >= 29 {
            return Err(Reason(format!(
                "destination x{n} is reserved by the rewrite sequence"
            )));
        }
        let tmp = if n != 16 { 16 } else { 17 };
        let p = &mut self.patch;
        let start = p.here();
        p.emit(a64::stur(tmp, 31, SPILL)?);
        p.slot_addr(n, tmp, key)?;
        p.emit(a64::ldr_slot(n, n, 0)?);
        p.emit(a64::ldur(tmp, 31, SPILL)?);
        p.emit_b(a + 4)?;
        Ok(start)
    }

    /// `msr tpidr_el0, xN` -> thread slot = xN.
    pub(crate) fn site_tpidr_write(&mut self, a: u32, n: u32) -> R<u32> {
        if n == 18 || n == 31 {
            return Err(Reason(format!(
                "source register {n} unsupported for msr tpidr_el0"
            )));
        }
        let t = a64::pick_scratch(1 << n, 1).ok_or("no scratch")?[0];
        let tmp = [16u32, 17]
            .into_iter()
            .find(|x| *x != n && *x != t)
            .unwrap();
        let p = &mut self.patch;
        let start = p.here();
        p.emit(a64::stur(t, 31, SPILL)?);
        p.emit(a64::stur(tmp, 31, SPILL - 8)?);
        p.slot_addr(t, tmp, Key::Ktls)?;
        p.emit(a64::str_slot(n, t, 0)?);
        p.emit(a64::ldur(tmp, 31, SPILL - 8)?);
        p.emit(a64::ldur(t, 31, SPILL)?);
        p.emit_b(a + 4)?;
        Ok(start)
    }

    /// Calls the host through the gate address in slot `KGATE`. `x29`/`x30` are saved on the stack
    /// by the caller of this helper; `x29` is borrowed for the key lookup and restored from the
    /// stack before the call so the host sees the guest's value (it walks the x29 chain).
    fn emit_gate_call(&mut self) -> R<()> {
        let p = &mut self.patch;
        p.slot_addr(30, 29, Key::Kgate)?;
        p.emit(a64::ldr_slot(30, 30, 0)?);
        p.emit(a64::ldr_slot(29, 31, 0)?); // ldr x29, [sp]
        p.emit(a64::blr(30));
        Ok(())
    }

    /// `svc #n`, `mrs xN, cntpct_el0` and friends: enter the host, then continue after the site.
    pub(crate) fn site_gate(&mut self, a: u32, kind: GateKind, arg: u32) -> R<u32> {
        if kind != GateKind::Svc && (arg == 18 || arg >= 29) {
            return Err(Reason(format!(
                "counter destination x{arg} cannot be written by the host gate"
            )));
        }
        let p = &mut self.patch;
        let start = p.here();
        // the host call needs stack: move sp below the guest's red zone first
        p.emit(a64::sub_imm(31, 31, (-SPILL) as u32, false));
        p.emit(a64::stp_pre(29, 30, 31, -16)?);
        self.emit_gate_call()?;
        let p = &mut self.patch;
        let ret = p.here();
        p.emit(a64::ldp_post(29, 30, 31, 16)?);
        p.emit(a64::add_imm(31, 31, (-SPILL) as u32, false));
        p.emit_b(a + 4)?;
        self.gates.push(Gate {
            ret,
            kind,
            arg,
            orig: a,
        });
        Ok(start)
    }
}
