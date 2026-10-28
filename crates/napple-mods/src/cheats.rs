//! Atmosphere-style cheat files and the opcode subset Napple defines (see docs/mods/format.md, "Cheat opcode subset").
//!
//! File: `[Name]` starts a cheat, `{Name}` a master code; each following line holds 8-digit hex words separated by spaces.
//! Supported opcodes (Atmosphere code types 0-7 in the forms listed in docs/mods/format.md; the rest decode as unsupported):
//!   `0TMR00AA AAAAAAAA VVVVVVVV (VVVVVVVV)`  store V (T bytes: 1, 2, 4, 8) at region M, address A (40 bit) + register R (R, zero until loaded)
//!   `1TMC00AA AAAAAAAA VVVVVVVV (VVVVVVVV)`  begin conditional: run the block when `mem[A] C V` (C: 1 >, 2 >=, 3 <, 4 <=, 5 ==, 6 !=)
//!   `21000000`                               else
//!   `20000000`                               end conditional
//! M: 0 main (module text/data), 1 heap, 2 alias, 3 aslr. Every other code type is decoded as unsupported and stops decoding.
use crate::util::{Result, err};

const MAX_CHEATS: usize = 4096;
const MAX_WORDS: usize = 1 << 16;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Cheat {
    pub name: String,
    pub master: bool,
    pub words: Vec<u32>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct CheatFile {
    pub cheats: Vec<Cheat>,
}

pub fn parse(text: &str) -> Result<CheatFile> {
    let mut f = CheatFile::default();
    for (n, raw) in text.lines().enumerate() {
        let ln = n + 1;
        let line = raw.trim();
        if line.is_empty() || line.starts_with("//") {
            continue;
        }
        let head = match line.as_bytes()[0] {
            b'[' => Some((b']', false)),
            b'{' => Some((b'}', true)),
            _ => None,
        };
        if let Some((close, master)) = head {
            let name = line[1..]
                .strip_suffix(close as char)
                .ok_or(crate::util::ModError(format!(
                    "line {ln}: unterminated name"
                )))?
                .trim();
            if name.is_empty() || name.len() > 64 {
                return err(format!("line {ln}: cheat name must be 1..=64 bytes"));
            }
            if f.cheats.len() >= MAX_CHEATS {
                return err("too many cheats");
            }
            f.cheats.push(Cheat {
                name: name.to_string(),
                master,
                words: vec![],
            });
            continue;
        }
        let Some(c) = f.cheats.last_mut() else {
            return err(format!("line {ln}: opcode line before any [name]"));
        };
        for tok in line.split_whitespace() {
            if tok.len() != 8 || !tok.bytes().all(|b| b.is_ascii_hexdigit()) {
                return err(format!("line {ln}: {tok:?} is not an 8-digit hex word"));
            }
            c.words.push(u32::from_str_radix(tok, 16).unwrap_or(0));
        }
        if c.words.len() > MAX_WORDS {
            return err(format!("line {ln}: cheat too long"));
        }
    }
    Ok(f)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Region {
    Main,
    Heap,
    Alias,
    Aslr,
    /// An absolute guest address (what a register holds after a pointer load); never written in a cheat file.
    Abs,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Cond {
    Gt,
    Ge,
    Lt,
    Le,
    Eq,
    Ne,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ArithOp {
    Add,
    Sub,
    Mul,
    Lsh,
    Rsh,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Op {
    /// `0TMR00AA ...`: address = region base + register `reg` (all zero until a cheat loads them) + `addr`.
    Store {
        width: u8,
        region: Region,
        addr: u64,
        value: u64,
        reg: u8,
    },
    If {
        width: u8,
        region: Region,
        addr: u64,
        cond: Cond,
        value: u64,
    },
    Else,
    End,
    /// `300R0000 VVVVVVVV`: `reg = count`; the body runs `count + 1` times (Atmosphere's loop counter behaviour).
    Loop {
        reg: u8,
        count: u32,
    },
    /// `310R0000`: if `reg != 0` then `reg -= 1` and jump back to the loop start.
    EndLoop {
        reg: u8,
    },
    /// `400R0000 VVVVVVVV VVVVVVVV`.
    LoadImm {
        reg: u8,
        value: u64,
    },
    /// `5TMR00AA AAAAAAAA` (`rel` false: region base + addr) or `5TMR10AA AAAAAAAA` (`rel` true: register `reg` + addr, absolute).
    LoadMem {
        width: u8,
        region: Region,
        reg: u8,
        addr: u64,
        rel: bool,
    },
    /// `6T0RIor0 VVVVVVVV VVVVVVVV`: store at absolute address `regs[reg]` (+ `regs[off]` when `off` is set); `inc` adds the width to `regs[reg]` after.
    StoreReg {
        width: u8,
        reg: u8,
        inc: bool,
        off: Option<u8>,
        value: u64,
    },
    /// `7T0RC000 VVVVVVVV (VVVVVVVV)`: `regs[reg] = regs[reg] op value`, truncated to the width.
    Arith {
        width: u8,
        reg: u8,
        op: ArithOp,
        value: u64,
    },
}

/// Decoded ops plus the reason decoding stopped early, if it did.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Decoded {
    pub ops: Vec<Op>,
    pub unsupported: Option<String>,
}

fn region(m: u32) -> Option<Region> {
    Some(match m {
        0 => Region::Main,
        1 => Region::Heap,
        2 => Region::Alias,
        3 => Region::Aslr,
        _ => return None,
    })
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Block {
    Cond,
    Loop(u8),
}

/// `Cheat` words to ops. Napple's opcode table is listed in docs/mods/format.md; keep the C port (`runtime/src/cheats.c`) in step with it.
pub fn decode(words: &[u32]) -> Decoded {
    let mut d = Decoded::default();
    let mut i = 0;
    let mut blocks: Vec<Block> = vec![];
    macro_rules! stop {
        ($($a:tt)*) => {{
            d.unsupported = Some(format!($($a)*));
            return d;
        }};
    }
    while i < words.len() {
        let w = words[i];
        let (code, t, m, x) = (w >> 28, (w >> 24) & 0xF, (w >> 20) & 0xF, (w >> 16) & 0xF);
        let need = |n: usize| i + n <= words.len();
        match code {
            0 | 1 => {
                let width = t as u8;
                if ![1, 2, 4, 8].contains(&width) {
                    stop!("word {i}: bad width {width}");
                }
                let n = if width == 8 { 4 } else { 3 };
                let Some(rg) = region(m) else {
                    stop!("word {i}: bad memory type {m}");
                };
                if !need(n) {
                    stop!("word {i}: truncated opcode");
                }
                if w & 0xFF00 != 0 {
                    stop!("word {i}: reserved bits set");
                }
                let addr = (((w & 0xFF) as u64) << 32) | words[i + 1] as u64;
                let mut value = words[i + 2] as u64;
                if width == 8 {
                    value = (value << 32) | words[i + 3] as u64;
                }
                if code == 0 {
                    d.ops.push(Op::Store {
                        width,
                        region: rg,
                        addr,
                        value,
                        reg: x as u8,
                    });
                } else {
                    let cond = match x {
                        1 => Cond::Gt,
                        2 => Cond::Ge,
                        3 => Cond::Lt,
                        4 => Cond::Le,
                        5 => Cond::Eq,
                        6 => Cond::Ne,
                        _ => stop!("word {i}: bad condition {x}"),
                    };
                    blocks.push(Block::Cond);
                    d.ops.push(Op::If {
                        width,
                        region: rg,
                        addr,
                        cond,
                        value,
                    });
                }
                i += n;
            }
            2 if w == 0x2000_0000 || w == 0x2100_0000 => {
                if blocks.last() != Some(&Block::Cond) {
                    stop!("word {i}: end/else without a conditional");
                }
                if w == 0x2000_0000 {
                    blocks.pop();
                    d.ops.push(Op::End);
                } else {
                    d.ops.push(Op::Else);
                }
                i += 1;
            }
            3 if w & 0xFF00_FFFF == 0x3000_0000 && need(2) => {
                let reg = x as u8;
                blocks.push(Block::Loop(reg));
                d.ops.push(Op::Loop {
                    reg,
                    count: words[i + 1],
                });
                i += 2;
            }
            3 if w & 0xFF00_FFFF == 0x3100_0000 => {
                if blocks.last() != Some(&Block::Loop(x as u8)) {
                    stop!("word {i}: loop end without its loop start");
                }
                blocks.pop();
                d.ops.push(Op::EndLoop { reg: x as u8 });
                i += 1;
            }
            4 if w & 0xFF00_FFFF == 0x4000_0000 && need(3) => {
                d.ops.push(Op::LoadImm {
                    reg: x as u8,
                    value: ((words[i + 1] as u64) << 32) | words[i + 2] as u64,
                });
                i += 3;
            }
            5 if (w >> 8) & 0xF == 0 && need(2) => {
                let width = t as u8;
                let rel = (w >> 12) & 0xF;
                if ![1, 2, 4, 8].contains(&width) || rel > 1 {
                    stop!("word {i}: bad load width or source");
                }
                let Some(rg) = region(m) else {
                    stop!("word {i}: bad memory type {m}");
                };
                if rel == 1 && m != 0 {
                    stop!("word {i}: register-relative load takes no memory type");
                }
                d.ops.push(Op::LoadMem {
                    width,
                    region: if rel == 1 { Region::Abs } else { rg },
                    reg: x as u8,
                    addr: (((w & 0xFF) as u64) << 32) | words[i + 1] as u64,
                    rel: rel == 1,
                });
                i += 2;
            }
            6 if m == 0
                && w & 0xF == 0
                && (w >> 12) & 0xF <= 1
                && ((w >> 8) & 0xF) <= 1
                && need(3) =>
            {
                let width = t as u8;
                if ![1, 2, 4, 8].contains(&width) {
                    stop!("word {i}: bad width {width}");
                }
                let o = (w >> 8) & 0xF;
                if o == 0 && (w >> 4) & 0xF != 0 {
                    stop!("word {i}: reserved bits set");
                }
                d.ops.push(Op::StoreReg {
                    width,
                    reg: x as u8,
                    inc: (w >> 12) & 0xF == 1,
                    off: (o == 1).then_some(((w >> 4) & 0xF) as u8),
                    value: ((words[i + 1] as u64) << 32) | words[i + 2] as u64,
                });
                i += 3;
            }
            7 if m == 0 && w & 0xFFF == 0 => {
                let width = t as u8;
                let op = match (w >> 12) & 0xF {
                    0 => ArithOp::Add,
                    1 => ArithOp::Sub,
                    2 => ArithOp::Mul,
                    3 => ArithOp::Lsh,
                    4 => ArithOp::Rsh,
                    c => stop!("word {i}: bad arithmetic op {c}"),
                };
                if ![1, 2, 4, 8].contains(&width) {
                    stop!("word {i}: bad width {width}");
                }
                let n = if width == 8 { 3 } else { 2 };
                if !need(n) {
                    stop!("word {i}: truncated opcode");
                }
                let mut value = words[i + 1] as u64;
                if width == 8 {
                    value = (value << 32) | words[i + 2] as u64;
                }
                d.ops.push(Op::Arith {
                    width,
                    reg: x as u8,
                    op,
                    value,
                });
                i += n;
            }
            // pause / resume the cheat process: nothing to do for a frame-synchronous runner
            0xF if w == 0xFF00_0000 || w == 0xFF10_0000 => i += 1,
            _ => stop!("word {i}: code type {code:X} ({w:08X}) is not in the supported subset"),
        }
    }
    if !blocks.is_empty() {
        d.unsupported = Some("unterminated conditional or loop block".into());
    }
    d
}

/// Memory seen by the cheat interpreter (the runtime supplies the real one; tests use a byte map).
pub trait CheatMemory {
    fn read(&mut self, region: Region, addr: u64, width: u8) -> Option<u64>;
    fn write(&mut self, region: Region, addr: u64, width: u8, value: u64) -> bool;
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct RunStats {
    pub stores: usize,
    pub failed_accesses: usize,
    /// The step budget ran out (a runaway loop): the rest of the cheat was skipped.
    pub aborted: bool,
}

/// Steps one run may take (loop bodies count each pass): bounds a hostile `FFFFFFFF`-iteration loop.
pub const MAX_STEPS: usize = 1 << 20;

fn trunc(v: u64, width: u8) -> u64 {
    if width >= 8 {
        v
    } else {
        v & ((1u64 << (8 * width as u32)) - 1)
    }
}

pub fn run(ops: &[Op], mem: &mut dyn CheatMemory) -> RunStats {
    let mut st = RunStats::default();
    let mut regs = [0u64; 16];
    let mut tops = [0usize; 16];
    // each frame: (parent_active, condition_result)
    let mut stack: Vec<(bool, bool)> = vec![];
    let active = |s: &[(bool, bool)]| s.last().is_none_or(|&(p, c)| p && c);
    let mut ip = 0;
    let mut steps = 0usize;
    while ip < ops.len() {
        steps += 1;
        if steps > MAX_STEPS {
            st.aborted = true;
            break;
        }
        let op = &ops[ip];
        ip += 1;
        match *op {
            Op::Store {
                width,
                region,
                addr,
                value,
                reg,
            } => {
                if active(&stack) {
                    let a = addr.wrapping_add(regs[reg as usize & 15]);
                    if mem.write(region, a, width, trunc(value, width)) {
                        st.stores += 1;
                    } else {
                        st.failed_accesses += 1;
                    }
                }
            }
            Op::If {
                width,
                region,
                addr,
                cond,
                value,
            } => {
                let parent = active(&stack);
                let mut res = false;
                if parent {
                    match mem.read(region, addr, width) {
                        Some(v) => {
                            let value = trunc(value, width);
                            res = match cond {
                                Cond::Gt => v > value,
                                Cond::Ge => v >= value,
                                Cond::Lt => v < value,
                                Cond::Le => v <= value,
                                Cond::Eq => v == value,
                                Cond::Ne => v != value,
                            }
                        }
                        None => st.failed_accesses += 1,
                    }
                }
                stack.push((parent, res));
            }
            Op::Else => {
                if let Some(f) = stack.last_mut() {
                    f.1 = !f.1;
                }
            }
            Op::End => {
                stack.pop();
            }
            Op::Loop { reg, count } => {
                if active(&stack) {
                    regs[reg as usize & 15] = count as u64;
                    tops[reg as usize & 15] = ip;
                }
            }
            Op::EndLoop { reg } => {
                let r = reg as usize & 15;
                if active(&stack) && regs[r] != 0 {
                    regs[r] -= 1;
                    ip = tops[r];
                }
            }
            Op::LoadImm { reg, value } => {
                if active(&stack) {
                    regs[reg as usize & 15] = value;
                }
            }
            Op::LoadMem {
                width,
                region,
                reg,
                addr,
                rel,
            } => {
                if active(&stack) {
                    let a = if rel {
                        regs[reg as usize & 15].wrapping_add(addr)
                    } else {
                        addr
                    };
                    match mem.read(region, a, width) {
                        Some(v) => regs[reg as usize & 15] = v,
                        None => st.failed_accesses += 1,
                    }
                }
            }
            Op::StoreReg {
                width,
                reg,
                inc,
                off,
                value,
            } => {
                if active(&stack) {
                    let r = reg as usize & 15;
                    let a = regs[r].wrapping_add(off.map_or(0, |o| regs[o as usize & 15]));
                    if mem.write(Region::Abs, a, width, trunc(value, width)) {
                        st.stores += 1;
                    } else {
                        st.failed_accesses += 1;
                    }
                    if inc {
                        regs[r] = regs[r].wrapping_add(width as u64);
                    }
                }
            }
            Op::Arith {
                width,
                reg,
                op,
                value,
            } => {
                if active(&stack) {
                    let r = reg as usize & 15;
                    let a = trunc(regs[r], width);
                    regs[r] = trunc(
                        match op {
                            ArithOp::Add => a.wrapping_add(value),
                            ArithOp::Sub => a.wrapping_sub(value),
                            ArithOp::Mul => a.wrapping_mul(value),
                            ArithOp::Lsh => a.checked_shl(value as u32).unwrap_or(0),
                            ArithOp::Rsh => a.checked_shr(value as u32).unwrap_or(0),
                        },
                        width,
                    );
                }
            }
        }
    }
    st
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeMap;

    #[derive(Default)]
    struct Mem(BTreeMap<u64, u8>);
    impl CheatMemory for Mem {
        fn read(&mut self, _r: Region, a: u64, w: u8) -> Option<u64> {
            let mut v = 0u64;
            for i in 0..w as u64 {
                v |= (*self.0.get(&(a + i))? as u64) << (8 * i);
            }
            Some(v)
        }
        fn write(&mut self, _r: Region, a: u64, w: u8, v: u64) -> bool {
            for i in 0..w as u64 {
                self.0.insert(a + i, (v >> (8 * i)) as u8);
            }
            true
        }
    }

    const FILE: &str = "[Infinite coins]\n04000000 00123456 0000270F\n\n{Master}\n04000000 00000010 00000001\n\
[Cond]\n14050000 00001000 00000001\n04000000 00002000 000000AA\n21000000\n04000000 00002000 000000BB\n20000000\n\
[Wide]\n08000000 00003000 11223344 55667788\n[Bad]\n80000000\n";

    #[test]
    fn parses_names_and_master() {
        let f = parse(FILE).unwrap();
        assert_eq!(f.cheats.len(), 5);
        assert!(f.cheats[1].master);
        assert_eq!(f.cheats[0].words, vec![0x04000000, 0x123456, 0x270F]);
    }

    #[test]
    fn decodes_and_runs_conditionals() {
        let f = parse(FILE).unwrap();
        let d = decode(&f.cheats[2].words);
        assert!(d.unsupported.is_none(), "{:?}", d.unsupported);
        let mut m = Mem::default();
        m.write(Region::Main, 0x1000, 4, 1);
        assert_eq!(run(&d.ops, &mut m).stores, 1);
        assert_eq!(m.0[&0x2000], 0xAA);
        m.write(Region::Main, 0x1000, 4, 2);
        run(&d.ops, &mut m);
        assert_eq!(m.0[&0x2000], 0xBB);
        let w = decode(&f.cheats[3].words);
        assert_eq!(
            w.ops,
            vec![Op::Store {
                width: 8,
                region: Region::Main,
                addr: 0x3000,
                value: 0x1122334455667788,
                reg: 0
            }]
        );
        assert!(decode(&f.cheats[4].words).unsupported.is_some());
    }

    #[derive(Default)]
    struct Mem2(BTreeMap<(u8, u64), u8>);
    fn rid(r: Region) -> u8 {
        r as u8
    }
    impl CheatMemory for Mem2 {
        fn read(&mut self, r: Region, a: u64, w: u8) -> Option<u64> {
            let mut v = 0u64;
            for i in 0..w as u64 {
                v |= (*self.0.get(&(rid(r), a + i))? as u64) << (8 * i);
            }
            Some(v)
        }
        fn write(&mut self, r: Region, a: u64, w: u8, v: u64) -> bool {
            for i in 0..w as u64 {
                self.0.insert((rid(r), a + i), (v >> (8 * i)) as u8);
            }
            true
        }
    }
    fn rd(m: &mut Mem2, r: Region, a: u64, w: u8) -> u64 {
        m.read(r, a, w).unwrap()
    }

    #[test]
    fn registers_loops_and_pointer_chains() {
        // r1 = 0x100; store 0xAB (1 byte) at main + r1 + 0x10 (offset register form)
        let d = decode(&[0x4001_0000, 0, 0x100, 0x0101_0000, 0x10, 0xAB]);
        assert_eq!(d.unsupported, None, "{d:?}");
        let mut m = Mem2::default();
        assert_eq!(run(&d.ops, &mut m).stores, 1);
        assert_eq!(rd(&mut m, Region::Main, 0x110, 1), 0xAB);

        // loop on r2 = 3 runs the body 4 times (the same address is written each pass)
        let w2 = [0x3002_0000, 3, 0x0401_0000, 0x10, 0x77, 0x3102_0000];
        let d = decode(&w2);
        assert_eq!(d.unsupported, None, "{d:?}");
        let mut m = Mem2::default();
        assert_eq!(run(&d.ops, &mut m).stores, 4);

        // store with increment: r6 = 0x30; an 8-byte store then a 1-byte store land at 0x30 and 0x38
        let w = [
            0x4006_0000,
            0,
            0x30,
            0x6806_1000,
            0,
            0x1234,
            0x6106_1000,
            0,
            0x99,
        ];
        let d = decode(&w);
        assert_eq!(d.unsupported, None, "{d:?}");
        let mut m = Mem2::default();
        assert_eq!(run(&d.ops, &mut m).stores, 2);
        assert_eq!(rd(&mut m, Region::Abs, 0x30, 8), 0x1234);
        assert_eq!(rd(&mut m, Region::Abs, 0x38, 1), 0x99);

        // store with an offset register: r7 = 0x100, r8 = 0x20 -> [0x120]
        let w = [
            0x4007_0000,
            0,
            0x100,
            0x4008_0000,
            0,
            0x20,
            0x6107_0180,
            0,
            0x5A,
        ];
        let d = decode(&w);
        assert_eq!(d.unsupported, None, "{d:?}");
        let mut m = Mem2::default();
        run(&d.ops, &mut m);
        assert_eq!(rd(&mut m, Region::Abs, 0x120, 1), 0x5A);

        // arithmetic wraps at the width: r5 = 0xFF, 1-byte +2 -> 1, and a shift by >= 64 clears
        let w = [
            0x4005_0000,
            0,
            0xFF,
            0x7105_0000,
            2,
            0x4006_0000,
            0,
            0x400,
            0x6806_0000,
            0,
            0,
        ];
        let d = decode(&w);
        assert_eq!(d.unsupported, None, "{d:?}");

        // pointer chain: heap[0x8] holds 0x5000; r1 = [heap+8]; r1 = [r1 + 4] (4 bytes); then compare through a conditional
        let w = [0x5811_0000, 0x8, 0x5401_1000, 0x4];
        let d = decode(&w);
        assert_eq!(d.unsupported, None, "{d:?}");
        let mut m = Mem2::default();
        m.write(Region::Heap, 0x8, 8, 0x5000);
        m.write(Region::Abs, 0x5004, 4, 0xDEAD);
        assert_eq!(run(&d.ops, &mut m).failed_accesses, 0);
        let mut m = Mem2::default(); // nothing mapped: both loads fail and are counted
        assert_eq!(run(&d.ops, &mut m).failed_accesses, 2);

        // a conditional that is false skips a loop whose end is also skipped
        let w = [
            0x1415_0000,
            0x1000,
            1,
            0x3002_0000,
            1000,
            0x3102_0000,
            0x2000_0000,
        ];
        let d = decode(&w);
        assert_eq!(d.unsupported, None, "{d:?}");
        let mut m = Mem2::default();
        m.write(Region::Heap, 0x1000, 4, 0);
        let st = run(&d.ops, &mut m);
        assert!(!st.aborted);
    }

    #[test]
    fn shared_vectors_decode() {
        let f = parse(include_str!(
            "../../../docs/mods/examples/cheat-vectors.txt"
        ))
        .unwrap();
        assert_eq!(f.cheats.len(), 7);
        for c in &f.cheats {
            let bad = decode(&c.words).unsupported;
            assert_eq!(
                bad.is_some(),
                c.name == "Unsupported",
                "{}: {bad:?}",
                c.name
            );
        }
    }

    #[test]
    fn runaway_loop_is_bounded() {
        let d = decode(&[0x3000_0000, u32::MAX, 0x7401_0000, 1, 0x3100_0000]);
        assert_eq!(d.unsupported, None);
        let st = run(&d.ops, &mut Mem2::default());
        assert!(st.aborted);
    }

    #[test]
    fn structure_errors() {
        assert!(decode(&[0x3000_0000, 2]).unsupported.is_some()); // loop never ended
        assert!(decode(&[0x3100_0000]).unsupported.is_some());
        assert!(decode(&[0x3000_0000, 2, 0x3101_0000]).unsupported.is_some()); // wrong register
        // end of a conditional inside a loop
        assert!(
            decode(&[0x3000_0000, 2, 0x2000_0000, 0x3100_0000])
                .unsupported
                .is_some()
        );
        assert!(decode(&[0x7900_0000, 1]).unsupported.is_some()); // bad width
        assert!(decode(&[0xA000_0000]).unsupported.is_some()); // not in the table
        assert_eq!(decode(&[0xFF00_0000, 0xFF10_0000]).unsupported, None); // pause / resume are no-ops
    }

    #[test]
    fn malformed() {
        assert!(parse("04000000").is_err());
        assert!(parse("[x\n").is_err());
        assert!(parse("[x]\nZZ000000\n").is_err());
        assert!(parse("[x]\n0400000\n").is_err());
        assert!(decode(&[0x0400_0000]).unsupported.is_some());
        assert!(decode(&[0x2000_0000]).unsupported.is_some());
        assert!(decode(&[0x1405_0000, 0, 0]).unsupported.is_some());
    }

    #[test]
    fn corpus_never_panics() {
        let mut s = 0xdead_beef_cafe_f00du64;
        for _ in 0..4000 {
            let mut m = FILE.as_bytes().to_vec();
            for _ in 0..(crate::util::xorshift(&mut s) % 5 + 1) {
                let i = crate::util::xorshift(&mut s) as usize % m.len();
                m[i] = crate::util::xorshift(&mut s) as u8;
            }
            if let Ok(f) = parse(&String::from_utf8_lossy(&m)) {
                for c in &f.cheats {
                    let d = decode(&c.words);
                    run(&d.ops, &mut Mem::default());
                }
            }
        }
        for _ in 0..4000 {
            let w: Vec<u32> = (0..(crate::util::xorshift(&mut s) % 12))
                .map(|_| crate::util::xorshift(&mut s) as u32 & 0x2FFF_FFFF)
                .collect();
            run(&decode(&w).ops, &mut Mem::default());
        }
    }
}
