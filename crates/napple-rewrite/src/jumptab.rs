//! Recovery of the extent of `br x18` jump tables.
//!
//! The rewritten dispatcher turns the (virtual) x18 target into an index into a run of landing
//! pads, so it needs the set of possible targets: `base + 4*i` for `i < count`. Two exact shapes
//! are recognised, and a function-wide range is the (always sound for compiler-generated
//! switches) fallback when the table base is not visible near the branch.
use crate::a64::{self, Text};
use crate::image::ModuleImage;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct JumpTable {
    pub base: u32,
    pub count: u32,
    pub how: Shape,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Shape {
    /// `adr x18, base; ldrb/ldrh idx; add x18, x18, idx, lsl #2; br x18`
    AdrByteHalf,
    /// `adrp/add T; cmp idx, #n; b.hi; ldrsw E,[T,idx,lsl #2]; add x18,E,T; br x18`
    SignedWord,
    /// Every instruction slot of the enclosing function (table base not recoverable locally).
    FunctionSpan,
}

struct Ins {
    addr: u32,
    word: u32,
    text: Option<Text>,
}

const NO_DEST: [&str; 22] = [
    "cmp", "cmn", "tst", "ccmp", "ccmn", "b", "bl", "br", "blr", "ret", "nop", "prfm", "prfum", "clrex", "svc", "msr", "fcmp", "fcmpe", "fccmp", "fccmpe", "dmb", "dsb",
];

fn is_flag_setter(m: &str) -> bool {
    ["cmp", "cmn", "tst", "adds", "subs", "ands", "bics", "adcs", "sbcs", "ccmp", "ccmn", "fcmp", "fcmpe", "fccmp", "fccmpe", "negs", "ngcs"].contains(&m)
}

/// Does `i` write general register `r` (as the first operand)? Stores and compares do not.
fn writes(i: &Ins, r: Option<u32>) -> bool {
    let (Some(r), Some(t)) = (r, &i.text) else { return false };
    let m = t.mnemonic.as_str();
    let store = m.starts_with("st") && !["stxr", "stlxr", "stxp", "stlxp", "stxrb", "stlxrb", "stxrh", "stlxrh"].contains(&m);
    if store || NO_DEST.contains(&m) || m.starts_with("b.") || m.starts_with("cb") || m.starts_with("tb") {
        return false;
    }
    t.ops.first().and_then(|o| a64::gpr_number(o)) == Some(r)
}

fn decode_range(img: &ModuleImage, from: u32, to: u32) -> Vec<Ins> {
    (from..to).step_by(4).map(|a| Ins { addr: a, word: img.word(a), text: a64::disasm(img.word(a)) }).collect()
}

fn op_num(t: &Text, k: usize) -> Option<u32> {
    t.ops.get(k).and_then(|o| a64::gpr_number(o))
}

fn imm_of(op: &str) -> Option<i64> {
    let s = op.strip_prefix('#')?;
    match s.strip_prefix("0x") {
        Some(h) => i64::from_str_radix(h, 16).ok(),
        None => s.parse().ok(),
    }
}

/// Table extent for the `br x18` at `br_at`; `func` is the enclosing function `[start, end)` if known.
pub fn analyze(img: &ModuleImage, br_at: u32, func: Option<(u32, u32)>, max_pads: u32) -> Result<JumpTable, String> {
    let lo = func.map_or(br_at.saturating_sub(4 * 48), |(f, _)| f.max(br_at.saturating_sub(4 * 48)));
    let ctx = decode_range(img, lo, br_at);
    if let Some(j) = adr_byte_half(&ctx) {
        return Ok(j);
    }
    if let Some(j) = signed_word(img, &ctx, func) {
        return Ok(j);
    }
    // No exact shape: accept only if the dispatch is recognisably a table walk, then bound by the function.
    if !looks_like_table_dispatch(&ctx) {
        return Err("br x18 is not a recognisable jump-table dispatch (tail call or unknown pattern)".into());
    }
    let (fa, fb) = func.ok_or("jump table base not visible and function bounds unknown")?;
    let count = (fb - fa) / 4;
    if count == 0 || count > max_pads {
        return Err(format!("function span of {count} slots exceeds the pad budget {max_pads}"));
    }
    Ok(JumpTable { base: fa, count, how: Shape::FunctionSpan })
}

/// `ldrsw E,[T,I,lsl #2]` feeding `add x18, E, T` somewhere before the branch, in straight-line code.
fn looks_like_table_dispatch(ctx: &[Ins]) -> bool {
    let Some(d) = last_x18_def(ctx) else { return false };
    let Some(t) = &ctx[d].text else { return false };
    if t.mnemonic != "add" || t.ops.len() != 3 || t.ops[0] != "x18" {
        return false;
    }
    let (a, b) = (op_num(t, 1), op_num(t, 2));
    ctx[..d].iter().rev().take_while(|i| !a64::is_control_flow(i.word)).any(|i| {
        i.text.as_ref().is_some_and(|x| {
            x.mnemonic == "ldrsw" && x.ops.len() == 4 && x.ops[3] == "lsl #2" && {
                let (e, tt) = (op_num(x, 0), op_num(x, 1));
                e.is_some() && tt.is_some() && e != tt && [e, tt].iter().all(|r| *r == a || *r == b)
            }
        })
    })
}

/// Index of the last instruction before the branch that writes x18; control flow in between aborts.
fn last_x18_def(ctx: &[Ins]) -> Option<usize> {
    for k in (0..ctx.len()).rev() {
        if a64::is_control_flow(ctx[k].word) {
            return None;
        }
        if writes(&ctx[k], Some(18)) {
            return Some(k);
        }
    }
    None
}

fn adr_byte_half(ctx: &[Ins]) -> Option<JumpTable> {
    let base = ctx.iter().rev().find(|i| i.word & 0x9F00_001F == 0x1000_0012).and_then(|i| a64::adr_target(i.word, i.addr as u64))?;
    // add x18, x18, xK, lsl #2
    let add = ctx.iter().rposition(|i| i.word & 0xFFE0_FFFF == 0x8B00_0A52)?;
    let idx = (ctx[add].word >> 16) & 31;
    let width = ctx[..add].iter().rev().find_map(|i| {
        let t = i.text.as_ref()?;
        if op_num(t, 0) != Some(idx) {
            return None;
        }
        match t.mnemonic.as_str() {
            "ldrb" | "ldurb" => Some(256u32),
            "ldrh" | "ldurh" => Some(65536),
            _ => None,
        }
    })?;
    Some(JumpTable { base: u32::try_from(base).ok()?, count: width, how: Shape::AdrByteHalf })
}

/// Reference-compatible analysis of old-clang signed-offset tables (see module docs).
fn signed_word(img: &ModuleImage, ctx: &[Ins], func: Option<(u32, u32)>) -> Option<JumpTable> {
    let d = last_x18_def(ctx)?;
    let t = ctx[d].text.as_ref()?;
    if t.mnemonic != "add" || t.ops.len() != 3 || t.ops[0] != "x18" {
        return None;
    }
    let (a, b) = (op_num(t, 1)?, op_num(t, 2)?);
    // the entry load
    let mut l = None;
    for k in (0..d).rev() {
        let p = &ctx[k];
        if let Some(x) = &p.text {
            if x.mnemonic == "ldrsw" && x.ops.len() == 4 && x.ops[3] == "lsl #2" {
                let (e, tb) = (op_num(x, 0), op_num(x, 1));
                if e.is_some() && tb.is_some() && e != tb && [e.unwrap(), tb.unwrap()].iter().all(|r| *r == a || *r == b) {
                    l = Some(k);
                    break;
                }
            }
        }
        if a64::is_control_flow(p.word) || writes(p, Some(a)) || writes(p, Some(b)) {
            return None;
        }
    }
    let k2 = l?;
    let lx = ctx[k2].text.as_ref()?;
    let (tr, ir) = (op_num(lx, 1)?, op_num(lx, 2)?);
    if ctx[k2 + 1..d].iter().any(|p| writes(p, Some(tr)) || writes(p, Some(ir))) {
        return None;
    }
    // table base: adrp xT, page ; add xT, xT, #lo
    let mut k3 = k2.checked_sub(1)?;
    while !writes(&ctx[k3], Some(tr)) {
        k3 = k3.checked_sub(1)?;
    }
    let add = ctx[k3].text.as_ref()?;
    if add.mnemonic != "add" || add.ops.len() != 3 || op_num(add, 0)? != tr || op_num(add, 1)? != tr {
        return None;
    }
    let lo = imm_of(&add.ops[2])?;
    let mut k4 = k3.checked_sub(1)?;
    while !writes(&ctx[k4], Some(tr)) {
        k4 = k4.checked_sub(1)?;
    }
    let adrp = ctx[k4].text.as_ref()?;
    if adrp.mnemonic != "adrp" || op_num(adrp, 0)? != tr {
        return None;
    }
    let table = a64::adr_target(ctx[k4].word, ctx[k4].addr as u64)? as i64 + lo;
    // bound: cmp wK, #n ; b.hi/b.hs default  (index possibly copied by mov in between)
    let mut src = vec![ir];
    let mut k5 = k2.checked_sub(1)?;
    loop {
        let p = &ctx[k5];
        let m = p.text.as_ref().map(|x| x.mnemonic.as_str()).unwrap_or("");
        if m == "b.hi" || m == "b.hs" {
            break;
        }
        if is_flag_setter(m) || a64::is_control_flow(p.word) {
            return None;
        }
        if m == "mov" {
            let x = p.text.as_ref()?;
            if let (Some(dst), Some(srcr)) = (op_num(x, 0), op_num(x, 1)) {
                if src.contains(&dst) {
                    src = vec![srcr];
                }
            }
        } else if src.iter().any(|r| writes(p, Some(*r))) {
            return None;
        }
        k5 = k5.checked_sub(1)?;
    }
    let hi = ctx[k5].text.as_ref()?.mnemonic == "b.hi";
    let mut k6 = k5.checked_sub(1)?;
    while !ctx[k6].text.as_ref().is_some_and(|x| is_flag_setter(&x.mnemonic)) {
        if src.iter().any(|r| writes(&ctx[k6], Some(*r))) || ctx[k6].text.as_ref().is_some_and(|x| x.mnemonic.starts_with('b')) {
            return None;
        }
        k6 = k6.checked_sub(1)?;
    }
    let cmp = ctx[k6].text.as_ref()?;
    if cmp.mnemonic != "cmp" || cmp.ops.len() != 2 || !src.contains(&op_num(cmp, 0)?) {
        return None;
    }
    let n = imm_of(&cmp.ops[1])? + hi as i64;
    if !(1..=4096).contains(&n) || table < 0 {
        return None;
    }
    let mut targets = Vec::with_capacity(n as usize);
    for e in 0..n as u32 {
        let off = (table as u32).checked_add(4 * e)?;
        if off as usize + 4 > img.layout.image_len {
            return None;
        }
        let t = (table + img.word(off) as i32 as i64) as u32 as i64;
        let inside = func.is_none_or(|(fa, fb)| (fa as i64..fb as i64).contains(&t));
        if !inside || t % 4 != 0 || t < 0 || t >= img.layout.text_end as i64 {
            return None;
        }
        targets.push(t as u32);
    }
    let (mn, mx) = (*targets.iter().min()?, *targets.iter().max()?);
    Some(JumpTable { base: mn, count: (mx - mn) / 4 + 1, how: Shape::SignedWord })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::scan::tests::module;

    #[test]
    fn adr_byte_table() {
        // 0: adr x18, +0x40 ; ldrb w9,[x8,x9]  ; add x18,x18,x9,lsl #2 ; br x18
        let img = module(&[0x1000_0212, 0x3869_6909, 0x8B09_0A52, 0xD61F_0240]);
        let j = analyze(&img, 12, Some((0, 16)), 1 << 17).unwrap();
        assert_eq!((j.base, j.count, j.how), (0x40, 256, Shape::AdrByteHalf));
    }

    #[test]
    fn tail_call_is_not_a_table() {
        // ldr x18,[x0,#8]; br x18
        let img = module(&[0xF940_0412, 0xD61F_0240]);
        assert!(analyze(&img, 4, Some((0, 8)), 1 << 17).is_err());
    }
}
