//! A64 helpers for the rewriter: word encoders, bit-level classification and a thin text
//! decoder (yaxpeax) used only to confirm which operands of an instruction are `x18`/`w18`.
//!
//! Everything that decides control flow or addresses is done on raw bits so the rewriter does not
//! depend on the decoder's alias choices; the decoder is only a second opinion for register use.
use std::fmt;
use yaxpeax_arch::{Decoder, U8Reader};
use yaxpeax_arm::armv8::a64::InstDecoder;

pub const NOP: u32 = 0xD503_201F;
pub const BLR_X18: u32 = 0xD63F_0240;
pub const BR_X18: u32 = 0xD61F_0240;

/// Error raised when a value does not fit an encoding (branch range, immediate width, ...).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EncodeError(pub String);

impl fmt::Display for EncodeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for EncodeError {}

type Enc = Result<u32, EncodeError>;

fn bad(what: &str, v: i64) -> EncodeError {
    EncodeError(format!("{what} out of range: {v:#x}"))
}

// ---------------------------------------------------------------- encoders

/// `b target` placed at `from`.
pub fn b(from: u64, to: u64) -> Enc {
    let d = to as i64 - from as i64;
    if d % 4 != 0 || !(-(1 << 27)..(1 << 27)).contains(&d) {
        return Err(bad("b displacement", d));
    }
    Ok(0x1400_0000 | ((d >> 2) as u32 & 0x03FF_FFFF))
}

fn pair(base: u32, rt: u32, rt2: u32, rn: u32, off: i32) -> Enc {
    if off % 8 != 0 || !(-512..=504).contains(&off) {
        return Err(bad("ldp/stp offset", off as i64));
    }
    Ok(base | (((off / 8) as u32 & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt)
}
pub fn stp(rt: u32, rt2: u32, rn: u32, off: i32) -> Enc {
    pair(0xA900_0000, rt, rt2, rn, off)
}
pub fn ldp(rt: u32, rt2: u32, rn: u32, off: i32) -> Enc {
    pair(0xA940_0000, rt, rt2, rn, off)
}
pub fn stp_pre(rt: u32, rt2: u32, rn: u32, off: i32) -> Enc {
    pair(0xA980_0000, rt, rt2, rn, off)
}
pub fn ldp_post(rt: u32, rt2: u32, rn: u32, off: i32) -> Enc {
    pair(0xA8C0_0000, rt, rt2, rn, off)
}

fn unscaled(base: u32, rt: u32, rn: u32, off: i32) -> Enc {
    if !(-256..=255).contains(&off) {
        return Err(bad("stur/ldur offset", off as i64));
    }
    Ok(base | ((off as u32 & 0x1FF) << 12) | (rn << 5) | rt)
}
pub fn stur(rt: u32, rn: u32, off: i32) -> Enc {
    unscaled(0xF800_0000, rt, rn, off)
}
pub fn ldur(rt: u32, rn: u32, off: i32) -> Enc {
    unscaled(0xF840_0000, rt, rn, off)
}

/// `ldr xt, [xn, #(slot * 8)]`
pub fn ldr_slot(rt: u32, rn: u32, slot: u32) -> Enc {
    if slot > 0xFFF {
        return Err(bad("ldr slot", slot as i64));
    }
    Ok(0xF940_0000 | (slot << 10) | (rn << 5) | rt)
}
/// `str xt, [xn, #(slot * 8)]`
pub fn str_slot(rt: u32, rn: u32, slot: u32) -> Enc {
    if slot > 0xFFF {
        return Err(bad("str slot", slot as i64));
    }
    Ok(0xF900_0000 | (slot << 10) | (rn << 5) | rt)
}

pub fn mrs_tpidrro(rt: u32) -> u32 {
    0xD53B_D060 | rt
}

pub fn adrp(rd: u32, pc: u64, target: u64) -> Enc {
    let pages = ((target & !0xFFF) as i64 - (pc & !0xFFF) as i64) >> 12;
    if !(-(1 << 20)..(1 << 20)).contains(&pages) {
        return Err(bad("adrp page distance", pages));
    }
    let imm = pages as u32 & 0x1F_FFFF;
    Ok(0x9000_0000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | rd)
}

/// `add xd, xn, #imm12 {, lsl #12}`
pub fn add_imm(rd: u32, rn: u32, imm12: u32, lsl12: bool) -> u32 {
    debug_assert!(imm12 <= 0xFFF);
    0x9100_0000 | ((lsl12 as u32) << 22) | (imm12 << 10) | (rn << 5) | rd
}
pub fn sub_imm(rd: u32, rn: u32, imm12: u32, lsl12: bool) -> u32 {
    debug_assert!(imm12 <= 0xFFF);
    0xD100_0000 | ((lsl12 as u32) << 22) | (imm12 << 10) | (rn << 5) | rd
}
/// `add xd, xn, xm`
pub fn add_reg(rd: u32, rn: u32, rm: u32) -> u32 {
    0x8B00_0000 | (rm << 16) | (rn << 5) | rd
}
pub fn br(rn: u32) -> u32 {
    0xD61F_0000 | (rn << 5)
}
pub fn blr(rn: u32) -> u32 {
    0xD63F_0000 | (rn << 5)
}

// ---------------------------------------------------------------- bit-level classification

/// Direct branch target of `b`, `bl`, `b.cond`, `cbz/cbnz`, `tbz/tbnz`, else `None`.
pub fn direct_target(word: u32, pc: u64) -> Option<u64> {
    let rel = |imm: i64| (pc as i64 + imm * 4) as u64;
    let sx = |v: u32, bits: u32| ((v << (32 - bits)) as i32 >> (32 - bits)) as i64;
    if word & 0x7C00_0000 == 0x1400_0000 {
        Some(rel(sx(word & 0x03FF_FFFF, 26)))
    } else if word & 0xFF00_0010 == 0x5400_0000 || word & 0x7E00_0000 == 0x3400_0000 {
        Some(rel(sx((word >> 5) & 0x7_FFFF, 19)))
    } else if word & 0x7E00_0000 == 0x3600_0000 {
        Some(rel(sx((word >> 5) & 0x3FFF, 14)))
    } else {
        None
    }
}

pub fn is_uncond_b(word: u32) -> bool {
    word & 0xFC00_0000 == 0x1400_0000
}
pub fn is_bl(word: u32) -> bool {
    word & 0xFC00_0000 == 0x9400_0000
}
pub fn is_cond_branch(word: u32) -> bool {
    word & 0xFF00_0010 == 0x5400_0000
        || word & 0x7E00_0000 == 0x3400_0000
        || word & 0x7E00_0000 == 0x3600_0000
}
/// `br`, `blr`, `ret` and the pointer-authenticated register branches.
pub fn is_indirect_branch(word: u32) -> bool {
    word & 0xFF9F_0000 == 0xD61F_0000 || word & 0xFF9F_0000 == 0xD71F_0000
}
/// `adr` / `adrp`.
pub fn is_adr_like(word: u32) -> bool {
    word & 0x1F00_0000 == 0x1000_0000
}
/// `ldr`/`ldrsw`/`prfm` with a literal (pc-relative) address, incl. SIMD literal loads.
pub fn is_literal_load(word: u32) -> bool {
    word & 0x3B00_0000 == 0x1800_0000
}
/// Load/store exclusive (not the ordered ldar/stlr or CAS forms, which have bit 23 set).
fn is_exclusive(word: u32) -> bool {
    word & 0x3F80_0000 == 0x0800_0000
}
pub fn is_excl_load(word: u32) -> bool {
    is_exclusive(word) && word & (1 << 22) != 0
}
pub fn is_excl_store(word: u32) -> bool {
    is_exclusive(word) && word & (1 << 22) == 0
}
/// Any instruction that ends a straight-line sequence or transfers control.
pub fn is_control_flow(word: u32) -> bool {
    direct_target(word, 0).is_some()
        || is_bl(word)
        || is_indirect_branch(word)
        || word & 0xFFE0_001F == 0xD400_0001
}

/// Replaces the branch displacement of a direct branch so it lands on `new_target`.
pub fn retarget(word: u32, pc: u64, new_target: u64) -> Enc {
    let d = new_target as i64 - pc as i64;
    if d % 4 != 0 {
        return Err(bad("branch displacement", d));
    }
    let w = d >> 2;
    let fit = |bits: u32| (-(1i64 << (bits - 1))..(1i64 << (bits - 1))).contains(&w);
    if word & 0x7C00_0000 == 0x1400_0000 {
        if !fit(26) {
            return Err(bad("b displacement", d));
        }
        Ok((word & 0xFC00_0000) | (w as u32 & 0x03FF_FFFF))
    } else if word & 0xFF00_0010 == 0x5400_0000 || word & 0x7E00_0000 == 0x3400_0000 {
        if !fit(19) {
            return Err(bad("conditional branch displacement", d));
        }
        Ok((word & 0xFF00_001F) | ((w as u32 & 0x7_FFFF) << 5))
    } else if word & 0x7E00_0000 == 0x3600_0000 {
        if !fit(14) {
            return Err(bad("tbz/tbnz displacement", d));
        }
        Ok((word & 0xFFF8_001F) | ((w as u32 & 0x3FFF) << 5))
    } else {
        Err(EncodeError(format!("not a direct branch: {word:08x}")))
    }
}

/// The four 5-bit register fields (Rd/Rt, Rn, Ra/Rt2, Rm) of a word, whatever the instruction means.
pub fn reg_fields(word: u32) -> [u32; 4] {
    [
        word & 31,
        (word >> 5) & 31,
        (word >> 10) & 31,
        (word >> 16) & 31,
    ]
}

/// Cheap prefilter: could this word name register 18 in any operand position?
pub fn may_use_reg18(word: u32) -> bool {
    reg_fields(word).contains(&18)
}

/// Target of `adr` / `adrp` (absolute address, page aligned for `adrp`).
pub fn adr_target(word: u32, pc: u64) -> Option<u64> {
    if !is_adr_like(word) {
        return None;
    }
    let imm = (((word >> 5) & 0x7_FFFF) << 2) | ((word >> 29) & 3);
    let imm = ((imm << 11) as i32 >> 11) as i64;
    Some(if word & 0x8000_0000 != 0 {
        ((pc & !0xFFF) as i64 + (imm << 12)) as u64
    } else {
        (pc as i64 + imm) as u64
    })
}

/// Target of a pc-relative literal load.
pub fn literal_target(word: u32, pc: u64) -> Option<u64> {
    if !is_literal_load(word) {
        return None;
    }
    let imm = ((((word >> 5) & 0x7_FFFF) << 13) as i32 >> 13) as i64;
    Some((pc as i64 + imm * 4) as u64)
}

// ---------------------------------------------------------------- text decoding

/// Decoded view of one instruction: mnemonic and comma-separated operands, from the decoder's text.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Text {
    pub mnemonic: String,
    pub ops: Vec<String>,
    pub full: String,
}

pub fn disasm(word: u32) -> Option<Text> {
    let bytes = word.to_le_bytes();
    let inst = InstDecoder::default()
        .decode(&mut U8Reader::new(&bytes))
        .ok()?;
    let full = inst.to_string();
    let (m, rest) = full.split_once(' ').unwrap_or((full.as_str(), ""));
    let ops = rest
        .replace(['[', ']', '{', '}', '!'], "")
        .split(',')
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .collect();
    Some(Text {
        mnemonic: m.to_string(),
        ops,
        full: full.clone(),
    })
}

fn tokens(text: &str) -> impl Iterator<Item = &str> {
    text.split(|c: char| !c.is_ascii_alphanumeric())
}

/// Does the decoded text name `x18`/`w18` as an operand?
pub fn text_names_reg18(text: &str) -> bool {
    tokens(text).any(|t| t == "x18" || t == "w18")
}

/// Register number of a `xN`/`wN` operand string.
pub fn gpr_number(op: &str) -> Option<u32> {
    let r = op.strip_prefix('x').or_else(|| op.strip_prefix('w'))?;
    r.parse::<u32>().ok().filter(|n| *n < 31)
}

/// Does this word use `x18`/`w18` (per the decoder)? Undecodable words answer `None`.
pub fn uses_reg18(word: u32) -> Option<bool> {
    if !may_use_reg18(word) {
        return Some(false);
    }
    disasm(word).map(|t| text_names_reg18(&t.full))
}

/// Rewrites every `x18`/`w18` operand of `word` to register `s`, confirmed by decoding the result.
///
/// Only the four register field positions are candidates; a candidate set is accepted when the
/// decoded text of the new word equals the original text with `18` replaced by `s`.
pub fn subst_reg18(word: u32, s: u32) -> Option<u32> {
    let orig = disasm(word)?;
    let want = replace_reg18(&orig.full, s);
    let positions: Vec<u32> = [0u32, 5, 10, 16]
        .into_iter()
        .filter(|p| (word >> p) & 31 == 18)
        .collect();
    for k in (1..=positions.len()).rev() {
        for mask in 1u32..(1 << positions.len()) {
            if mask.count_ones() as usize != k {
                continue;
            }
            let mut nw = word;
            for (i, p) in positions.iter().enumerate() {
                if mask >> i & 1 == 1 {
                    nw = (nw & !(31 << p)) | (s << p);
                }
            }
            if disasm(nw).is_some_and(|t| t.full == want) {
                return Some(nw);
            }
        }
    }
    None
}

fn replace_reg18(text: &str, s: u32) -> String {
    let mut out = String::with_capacity(text.len());
    let mut chars = text.char_indices().peekable();
    let b = text.as_bytes();
    while let Some((i, c)) = chars.next() {
        let boundary_before = i == 0 || !b[i - 1].is_ascii_alphanumeric();
        if boundary_before
            && (c == 'x' || c == 'w')
            && text[i + 1..].starts_with("18")
            && !text[i + 3..].starts_with(|ch: char| ch.is_ascii_alphanumeric())
        {
            out.push(c);
            out.push_str(&s.to_string());
            chars.next();
            chars.next();
        } else {
            out.push(c);
        }
    }
    out
}

/// Registers a rewrite block must not borrow for `word`: all field values, and the next register of
/// each (pair instructions name only the first register of a register pair).
pub fn busy_regs(word: u32) -> u32 {
    let mut m = 0u32;
    for f in reg_fields(word) {
        m |= 1 << f;
        m |= 1 << (f + 1).min(31);
    }
    m
}

/// Picks `n` scratch registers from x0..x15 that are not in `busy` (bit mask) and are not x18.
pub fn pick_scratch(busy: u32, n: usize) -> Option<Vec<u32>> {
    let v: Vec<u32> = (0u32..16).filter(|r| busy >> r & 1 == 0).take(n).collect();
    (v.len() == n).then_some(v)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn branch_targets_round_trip() {
        let w = b(0x1000, 0x2000).unwrap();
        assert_eq!(direct_target(w, 0x1000), Some(0x2000));
        let w = b(0x2000, 0x1000).unwrap();
        assert_eq!(direct_target(w, 0x2000), Some(0x1000));
        assert!(b(0, 1 << 28).is_err());
        let cbz = 0xB400_0052u32; // cbz x18, +8
        assert_eq!(direct_target(cbz, 0x100), Some(0x108));
        assert_eq!(
            direct_target(retarget(cbz, 0x100, 0x200).unwrap(), 0x100),
            Some(0x200)
        );
        let tbz = 0x3600_0052u32; // tbz w18, #0, +8
        assert_eq!(
            direct_target(retarget(tbz, 0x100, 0x80).unwrap(), 0x100),
            Some(0x80)
        );
        let bne = 0x5400_0041u32;
        assert_eq!(
            direct_target(retarget(bne, 0x100, 0x1100).unwrap(), 0x100),
            Some(0x1100)
        );
    }

    #[test]
    fn pcrel_targets() {
        assert_eq!(adr_target(0x1000_0072, 0x100), Some(0x10C)); // adr x18, +0xc
        assert_eq!(adr_target(0x9000_0012, 0x1234), Some(0x1000)); // adrp x18, +0
        assert_eq!(literal_target(0x5800_ab12, 0x100), Some(0x100 + 0x1560));
        // pc-relative encoders agree with the decoder
        let w = adrp(5, 0x4000_0100, 0x4003_2000).unwrap();
        assert_eq!(adr_target(w, 0x4000_0100), Some(0x4003_2000));
    }

    #[test]
    fn exclusive_classification() {
        assert!(is_excl_load(0xC85F_7E40)); // ldxr x0,[x18]
        assert!(is_excl_load(0xC85F_FE52)); // ldaxr x18,[x18]
        assert!(is_excl_store(0xC802_7E41)); // stxr w2,x1,[x18]
        assert!(!is_exclusive(0xC8DF_FE52)); // ldar is ordered, not exclusive
    }

    #[test]
    fn reg18_substitution_checks_decoded_text() {
        for (w, s, want) in [
            (0xF940_0250u32, 3u32, "ldr x16, [x3]"),
            (0xA940_0A52, 4, "ldp x4, x2, [x4]"),
            (0x8B12_0A52, 1, "add x1, x1, x1, lsl #2"),
            (0xAA12_03E0, 7, "mov x0, x7"),
            (0x1B12_7C00, 9, "mul w0, w0, w9"),
            (0xF872_6A40, 5, "ldr x0, [x5, x5]"),
        ] {
            let nw = subst_reg18(w, s).unwrap_or_else(|| panic!("{w:08x}"));
            assert_eq!(disasm(nw).unwrap().full, want);
        }
        // prefetch hint field 18 is not a register: x18 only appears as base
        assert!(uses_reg18(0xF980_0A52).is_some());
        assert_eq!(uses_reg18(0x8B03_0041), Some(false));
        assert_eq!(uses_reg18(0xD503_201F), Some(false));
    }

    #[test]
    fn replace_respects_word_boundaries() {
        assert_eq!(replace_reg18("add x18, x18, #0x18", 3), "add x3, x3, #0x18");
        assert_eq!(replace_reg18("ldr x1, [x180]", 3), "ldr x1, [x180]");
    }

    #[test]
    fn scratch_selection_avoids_busy_registers() {
        let busy = busy_regs(0xF940_0250); // ldr x16,[x18]
        let s = pick_scratch(busy, 2).unwrap();
        assert_eq!(s.len(), 2);
        assert!(s.iter().all(|r| busy >> r & 1 == 0));
        let busy = busy_regs(0xA940_0A52 /* ldp x18, x2, [x18] */);
        assert!(!pick_scratch(busy, 2).unwrap().contains(&2));
    }
}
