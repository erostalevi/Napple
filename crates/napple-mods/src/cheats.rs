//! Atmosphere-style cheat files and the opcode subset Napple defines (see docs/mods/format.md, "Cheat opcode subset").
//!
//! File: `[Name]` starts a cheat, `{Name}` a master code; each following line holds 8-digit hex words separated by spaces.
//! Supported opcodes (compatible with Atmosphere code types 0, 1 and 2):
//!   `0TMR00AA AAAAAAAA VVVVVVVV (VVVVVVVV)`  store V (T bytes: 1, 2, 4, 8) at region M, address A (40 bit); R must be 0
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

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Op {
    Store {
        width: u8,
        region: Region,
        addr: u64,
        value: u64,
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

pub fn decode(words: &[u32]) -> Decoded {
    let mut d = Decoded::default();
    let mut i = 0;
    let mut depth = 0i32;
    while i < words.len() {
        let w = words[i];
        let (code, t, m, x) = (w >> 28, (w >> 24) & 0xF, (w >> 20) & 0xF, (w >> 16) & 0xF);
        match code {
            0 | 1 => {
                let width = t as u8;
                if ![1, 2, 4, 8].contains(&width) {
                    d.unsupported = Some(format!("word {i}: bad width {width}"));
                    return d;
                }
                let n = if width == 8 { 4 } else { 3 };
                let Some(rg) = region(m) else {
                    d.unsupported = Some(format!("word {i}: bad memory type {m}"));
                    return d;
                };
                if i + n > words.len() {
                    d.unsupported = Some(format!("word {i}: truncated opcode"));
                    return d;
                }
                let addr = (((w & 0xFF) as u64) << 32) | words[i + 1] as u64;
                let mut value = words[i + 2] as u64;
                if width == 8 {
                    value = (value << 32) | words[i + 3] as u64;
                }
                if code == 0 {
                    if x != 0 {
                        d.unsupported =
                            Some(format!("word {i}: offset registers are not supported"));
                        return d;
                    }
                    d.ops.push(Op::Store {
                        width,
                        region: rg,
                        addr,
                        value,
                    });
                } else {
                    let cond = match x {
                        1 => Cond::Gt,
                        2 => Cond::Ge,
                        3 => Cond::Lt,
                        4 => Cond::Le,
                        5 => Cond::Eq,
                        6 => Cond::Ne,
                        _ => {
                            d.unsupported = Some(format!("word {i}: bad condition {x}"));
                            return d;
                        }
                    };
                    depth += 1;
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
                if depth == 0 {
                    d.unsupported = Some(format!("word {i}: end/else without a conditional"));
                    return d;
                }
                if w == 0x2000_0000 {
                    depth -= 1;
                    d.ops.push(Op::End);
                } else {
                    d.ops.push(Op::Else);
                }
                i += 1;
            }
            _ => {
                d.unsupported = Some(format!(
                    "word {i}: code type {code:X} ({w:08X}) is not in the supported subset"
                ));
                return d;
            }
        }
    }
    if depth != 0 {
        d.unsupported = Some("unterminated conditional block".into());
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
}

pub fn run(ops: &[Op], mem: &mut dyn CheatMemory) -> RunStats {
    let mut st = RunStats::default();
    // each frame: (parent_active, condition_result)
    let mut stack: Vec<(bool, bool)> = vec![];
    let active = |s: &[(bool, bool)]| s.last().is_none_or(|&(p, c)| p && c);
    for op in ops {
        match *op {
            Op::Store {
                width,
                region,
                addr,
                value,
            } => {
                if active(&stack) {
                    if mem.write(region, addr, width, value) {
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
                value: 0x1122334455667788
            }]
        );
        assert!(decode(&f.cheats[4].words).unsupported.is_some());
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
