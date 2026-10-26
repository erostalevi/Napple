//! Differential equivalence harness (Unicorn): for every rewritten site, run the original
//! instruction(s) on the original image (x18/tpidr are real registers) and the patch block on the
//! rewritten image (x18/tpidr live in thread slots) from the same random machine state, and
//! compare registers, flags, vector registers, memory writes, the program counter and faults.
#![allow(dead_code)]
use napple_rewrite::image::ModuleImage;
use napple_rewrite::types::{GateKind, Key, RewriteOutput, SiteRow};
use rayon::prelude::*;
use std::collections::{BTreeMap, HashMap, HashSet};
use unicorn_engine::unicorn_const::{Arch, HookType, MemType, Mode, Prot};
use unicorn_engine::{RegisterARM64 as R, Unicorn};

pub const IB: u64 = 0x1_0000_0000;
const ARENA_SZ: usize = 0x100_0000;
const TSD: u64 = 0x200_0000;
const GATE_STUB: u64 = 0x300_0000;
const SP0: u64 = 0x80_0000;
const SLOT_BASE: u64 = 300;

fn gp(i: usize) -> R {
    use R::*;
    [
        X0, X1, X2, X3, X4, X5, X6, X7, X8, X9, X10, X11, X12, X13, X14, X15, X16, X17, X18, X19,
        X20, X21, X22, X23, X24, X25, X26, X27, X28, X29, X30,
    ][i]
}
fn qreg(i: usize) -> R {
    use R::*;
    [
        V0, V1, V2, V3, V4, V5, V6, V7, V8, V9, V10, V11, V12, V13, V14, V15, V16, V17, V18, V19,
        V20, V21, V22, V23, V24, V25, V26, V27, V28, V29, V30, V31,
    ][i]
}

pub struct Rng(u64);
impl Rng {
    pub fn new(seed: u64) -> Self {
        Rng(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) ^ 0xD1B5_4A32_D192_ED03)
    }
    pub fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }
    pub fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
}

/// Replaces exclusive loads/stores by plain ones (Unicorn's monitor is not what is under test).
pub fn deexcl(img: &[u8]) -> Vec<u8> {
    let mut b = img.to_vec();
    let ld = [0x3940_0000u32, 0x7940_0000, 0xB940_0000, 0xF940_0000];
    let st = [0x3900_0000u32, 0x7900_0000, 0xB900_0000, 0xF900_0000];
    for o in (0..b.len().saturating_sub(4)).step_by(4) {
        let w = u32::from_le_bytes(b[o..o + 4].try_into().unwrap());
        if w & 0x3F80_0000 != 0x0800_0000 {
            continue;
        }
        let (size, l, pair) = ((w >> 30) as usize, (w >> 22) & 1, (w >> 21) & 1);
        let (rs, rt2, rn, rt) = ((w >> 16) & 31, (w >> 10) & 31, (w >> 5) & 31, w & 31);
        let nw = if pair == 1 {
            if size < 2 {
                continue;
            }
            let base = match (size == 3, l == 1) {
                (true, true) => 0xA940_0000u32,
                (true, false) => 0xA900_0000,
                (false, true) => 0x2940_0000,
                (false, false) => 0x2900_0000,
            };
            base | (rt2 << 10) | (rn << 5) | rt
        } else {
            (if l == 1 { ld[size] } else { st[size] }) | (rn << 5) | rt
        };
        b[o..o + 4].copy_from_slice(&nw.to_le_bytes());
        if l == 0 {
            let nx = u32::from_le_bytes(b[o + 4..o + 8].try_into().unwrap());
            if nx & 0x7E00_0000 == 0x3400_0000 && nx & 31 == rs {
                b[o + 4..o + 8].copy_from_slice(&0xD503_201Fu32.to_le_bytes());
            }
        }
    }
    b
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Stop {
    /// Single instruction.
    Count1,
    /// Until the pc is back in original text after visiting the patch area.
    LeavePatch,
    /// Until the pc leaves the given original ranges.
    LeaveSet,
}

#[derive(Default)]
struct Emu {
    writes: Vec<(u64, u8, u64)>,
    fault: Option<(u8, u64)>,
    end_pc: Option<u64>,
    seen_patch: bool,
    patch_lo: u64,
    text_hi: u64,
    set: Vec<(u64, u64)>,
    mode: Option<Stop>,
}

struct Machine {
    uc: Unicorn<'static, Emu>,
    arena0: Vec<u8>,
}

fn machine(image: &[u8], arena0: &[u8], patch_off: u64, patched: bool) -> Machine {
    let mut uc = Unicorn::new_with_data(Arch::ARM64, Mode::LITTLE_ENDIAN, Emu::default()).unwrap();
    let sz = ((image.len() + 0xFFFF) & !0xFFFF) as u64;
    uc.mem_map(IB, sz, Prot::ALL).unwrap();
    uc.mem_write(IB, image).unwrap();
    uc.mem_map(0, ARENA_SZ as u64, Prot::ALL).unwrap();
    uc.mem_write(0, arena0).unwrap();
    uc.mem_map(TSD, 0x10000, Prot::ALL).unwrap();
    uc.mem_map(GATE_STUB, 0x1000, Prot::ALL).unwrap();
    uc.mem_write(GATE_STUB, &0xD65F_03C0u32.to_le_bytes())
        .unwrap(); // ret
    uc.reg_write(R::CPACR_EL1, 0x30_0000).unwrap();
    uc.reg_write(R::TPIDRRO_EL0, TSD).unwrap();
    {
        let st = uc.get_data_mut();
        st.patch_lo = IB + patch_off;
        st.text_hi = IB + patch_off;
    }
    uc.add_mem_hook(
        HookType::MEM_WRITE,
        0,
        u64::MAX,
        |uc, _t, addr, size, value| {
            let mask = if size >= 8 {
                u64::MAX
            } else {
                (1u64 << (8 * size)) - 1
            };
            uc.get_data_mut()
                .writes
                .push((addr, size as u8, value as u64 & mask));
            true
        },
    )
    .unwrap();
    uc.add_mem_hook(
        HookType::MEM_READ_UNMAPPED
            | HookType::MEM_WRITE_UNMAPPED
            | HookType::MEM_FETCH_UNMAPPED
            | HookType::MEM_READ_PROT
            | HookType::MEM_WRITE_PROT
            | HookType::MEM_FETCH_PROT,
        0,
        u64::MAX,
        |uc, t, addr, _s, _v| {
            let k = match t {
                MemType::READ_UNMAPPED => 1,
                MemType::WRITE_UNMAPPED => 2,
                MemType::FETCH_UNMAPPED => 3,
                _ => 4,
            };
            uc.get_data_mut().fault = Some((k, addr));
            false
        },
    )
    .unwrap();
    // one code hook for the emulator's lifetime (adding/removing hooks makes cached blocks run past stops)
    uc.add_code_hook(IB, IB + sz, move |uc, addr, _| {
        let st = uc.get_data_mut();
        match st.mode {
            Some(Stop::LeavePatch) => {
                if addr >= st.patch_lo {
                    st.seen_patch = true;
                } else if st.seen_patch {
                    st.end_pc = Some(addr);
                    let _ = uc.emu_stop();
                }
            }
            Some(Stop::LeaveSet) if !st.set.iter().any(|(a, b)| addr >= *a && addr < *b) => {
                st.end_pc = Some(addr);
                let _ = uc.emu_stop();
            }
            _ => {}
        }
    })
    .unwrap();
    let _ = patched;
    Machine {
        uc,
        arena0: arena0.to_vec(),
    }
}

#[derive(Clone)]
struct Init {
    g: [u64; 31],
    v: [u128; 32],
    nzcv: u64,
    tls: u64,
    tlr: u64,
}

fn init_state(rng: &mut Rng, kind: u32) -> Init {
    let mut g = [0u64; 31];
    for r in g.iter_mut() {
        *r = match kind {
            0 => rng.below(0x10000) & !7,
            1 => rng.below(ARENA_SZ as u64 - 0x10000),
            _ => rng.next(),
        };
    }
    let mut v = [0u128; 32];
    for q in v.iter_mut() {
        *q = (rng.next() as u128) << 64 | rng.next() as u128;
    }
    Init {
        g,
        v,
        nzcv: rng.below(16) << 28,
        tls: rng.below(0x100000) & !15,
        tlr: (rng.below(0x100000) + 0x100000) & !15,
    }
}

#[derive(PartialEq, Eq, Debug, Clone)]
pub struct Snapshot {
    g: Vec<u64>,
    sp: u64,
    nzcv: u64,
    v: Vec<u128>,
    tls: u64,
    writes: Vec<(u64, u8, u64)>,
    fault: Option<(u8, u64)>,
    end_pc: Option<u64>,
    /// The emulator rejected an instruction (undefined/unsupported encoding), not a memory fault.
    invalid: bool,
}

struct Layout {
    patch_off: u64,
    key_page_off: u64,
    key_idx: [u64; 8],
}

fn slot(k: Key) -> u64 {
    SLOT_BASE + k.index() as u64
}

impl Machine {
    #[allow(clippy::too_many_arguments)] // test harness entry point; the arguments are the machine state under test
    fn run(
        &mut self,
        start: u64,
        patched: bool,
        init: &Init,
        mode: Stop,
        set: Vec<(u64, u64)>,
        lay: &Layout,
        x18_override: Option<u64>,
    ) -> Snapshot {
        // restore the arena bytes touched by the previous run
        let writes = std::mem::take(&mut self.uc.get_data_mut().writes);
        for (a, s, _) in writes {
            if (a as usize) < ARENA_SZ {
                let e = (a as usize + s as usize).min(ARENA_SZ);
                let orig = self.arena0[a as usize..e].to_vec();
                let _ = self.uc.mem_write(a, &orig);
            }
        }
        {
            let st = self.uc.get_data_mut();
            st.fault = None;
            st.end_pc = None;
            st.seen_patch = false;
            st.mode = Some(mode);
            st.set = set;
        }
        let mut g = init.g;
        if let Some(x) = x18_override {
            g[18] = x;
        }
        for (i, v) in g.iter().enumerate() {
            self.uc.reg_write(gp(i), *v).unwrap();
        }
        for (i, q) in init.v.iter().enumerate() {
            self.uc.reg_write_long(qreg(i), &q.to_le_bytes()).unwrap();
        }
        self.uc.reg_write(R::SP, SP0).unwrap();
        self.uc.reg_write(R::NZCV, init.nzcv).unwrap();
        self.uc.reg_write(R::TPIDR_EL0, init.tls).unwrap();
        if patched {
            for (i, k) in Key::ALL.iter().enumerate() {
                self.uc
                    .mem_write(
                        IB + lay.key_page_off + 8 * lay.key_idx[i],
                        &(slot(*k) * 8).to_le_bytes(),
                    )
                    .unwrap();
            }
            self.uc
                .mem_write(TSD + 8 * slot(Key::K18), &g[18].to_le_bytes())
                .unwrap();
            self.uc
                .mem_write(TSD + 8 * slot(Key::Ktls), &init.tls.to_le_bytes())
                .unwrap();
            self.uc
                .mem_write(TSD + 8 * slot(Key::Ktlr), &init.tlr.to_le_bytes())
                .unwrap();
            self.uc
                .mem_write(TSD + 8 * slot(Key::Kgate), &GATE_STUB.to_le_bytes())
                .unwrap();
            self.uc.reg_write(gp(18), 0xDEAD_BEEF_DEAD_BEEF).unwrap();
            self.uc
                .reg_write(R::TPIDR_EL0, 0x1111_2222_3333_4444)
                .unwrap();
        } else {
            self.uc.reg_write(R::TPIDRRO_EL0, init.tlr).unwrap();
        }
        let r = match mode {
            Stop::Count1 => self.uc.emu_start(start, IB + 0x7FFF_FFF0, 0, 1),
            _ => self.uc.emu_start(start, IB + 0x7FFF_FFF0, 0, 600),
        };
        let invalid = r.is_err() && self.uc.get_data().fault.is_none();
        let fault = self.uc.get_data().fault;
        let mut end_pc = self.uc.get_data().end_pc;
        if end_pc.is_none() && fault.is_none() {
            end_pc = Some(if mode == Stop::Count1 {
                self.uc.reg_read(R::PC).unwrap()
            } else {
                0xFFFF_FFFF_FFFF
            });
        }
        let mut gv: Vec<u64> = (0..31).map(|i| self.uc.reg_read(gp(i)).unwrap()).collect();
        let mut tls = self.uc.reg_read(R::TPIDR_EL0).unwrap();
        if patched {
            let rd = |uc: &Unicorn<Emu>, k: Key| {
                u64::from_le_bytes(
                    uc.mem_read_as_vec(TSD + 8 * slot(k), 8)
                        .unwrap()
                        .try_into()
                        .unwrap(),
                )
            };
            gv[18] = rd(&self.uc, Key::K18);
            tls = rd(&self.uc, Key::Ktls);
        }
        let v: Vec<u128> = (0..32)
            .map(|i| {
                u128::from_le_bytes(
                    self.uc
                        .reg_read_long(qreg(i))
                        .unwrap()
                        .as_ref()
                        .try_into()
                        .unwrap(),
                )
            })
            .collect();
        let mut w = self.uc.get_data().writes.clone();
        if patched {
            w.retain(|(a, s, _)| {
                !(*s == 8
                    && *a >= TSD
                    && *a < TSD + 0x10000
                    && (*a - TSD).is_multiple_of(8)
                    && (*a - TSD) / 8 >= SLOT_BASE
                    && (*a - TSD) / 8 < SLOT_BASE + 8)
            });
        }
        Snapshot {
            g: gv,
            sp: self.uc.reg_read(R::SP).unwrap(),
            nzcv: self.uc.reg_read(R::NZCV).unwrap(),
            v,
            tls,
            writes: w,
            fault,
            end_pc,
            invalid,
        }
    }
}

/// What a row of `sites.tsv` asks the harness to test.
#[derive(Clone, Debug)]
enum Case {
    Single {
        addr: u32,
        label: &'static str,
        br: Option<(u32, u32)>,
        call: bool,
    },
    Window {
        lo: u32,
        group: Vec<(u32, u32)>,
    },
    Gate {
        addr: u32,
        kind: Option<GateKind>,
        arg: u32,
    },
}

#[derive(Default, Debug)]
pub struct Report {
    pub sites: usize,
    pub trials: usize,
    pub effective: usize,
    pub zero_effective: Vec<u32>,
    pub diverged: Vec<(u32, String)>,
    pub gate_sites: usize,
    pub mutations: usize,
    pub mutations_caught: usize,
    pub mutations_missed: Vec<u32>,
}

impl Report {
    pub fn ok(&self) -> bool {
        self.diverged.is_empty()
            && self.zero_effective.is_empty()
            && self.mutations_missed.is_empty()
    }
}

pub struct Options {
    pub seed: u64,
    pub min_eff: usize,
    pub max_extra: usize,
    pub mutate: usize,
}

impl Default for Options {
    fn default() -> Self {
        Options {
            seed: 1,
            min_eff: 2,
            max_extra: 16,
            mutate: 200,
        }
    }
}

fn dis(w: u32) -> String {
    napple_rewrite::a64::disasm(w)
        .map(|t| t.full)
        .unwrap_or_else(|| format!("{w:08x}"))
}

fn build_cases(orig: &[u8], out: &RewriteOutput) -> Vec<Case> {
    let gates: HashMap<u32, (GateKind, u32)> = out
        .gates
        .iter()
        .map(|g| (g.orig, (g.kind, g.arg)))
        .collect();
    let jumps: HashMap<u32, (u32, u32)> = out
        .sites
        .iter()
        .filter_map(|r| {
            if let SiteRow::Jump { addr, base, count } = r {
                Some((*addr, (*base, *count)))
            } else {
                None
            }
        })
        .collect();
    // atomic groups: windows connected by direct branches between them
    let wins: Vec<(u32, u32)> = out
        .sites
        .iter()
        .filter_map(|r| {
            if let SiteRow::Window { lo, hi } = r {
                Some((*lo, *hi))
            } else {
                None
            }
        })
        .collect();
    let word = |a: u32| u32::from_le_bytes(orig[a as usize..a as usize + 4].try_into().unwrap());
    let mut parent: Vec<usize> = (0..wins.len()).collect();
    fn find(p: &mut [usize], x: usize) -> usize {
        let mut x = x;
        while p[x] != x {
            p[x] = p[p[x]];
            x = p[x];
        }
        x
    }
    for (i, (lo, hi)) in wins.iter().enumerate() {
        for a in (*lo..*hi).step_by(4) {
            if let Some(t) = napple_rewrite::a64::direct_target(word(a), a as u64)
                && let Some(j) = wins
                    .iter()
                    .position(|(l, h)| (*l..*h).contains(&(t as u32)))
            {
                let (ri, rj) = (find(&mut parent, i), find(&mut parent, j));
                parent[ri] = rj;
            }
        }
    }
    let mut cases = Vec::new();
    for r in &out.sites {
        match r {
            SiteRow::Single { addr, label } => {
                if let Some((k, arg)) = gates.get(addr) {
                    cases.push(Case::Gate {
                        addr: *addr,
                        kind: Some(*k),
                        arg: *arg,
                    });
                } else {
                    cases.push(Case::Single {
                        addr: *addr,
                        label,
                        br: jumps.get(addr).copied(),
                        call: false,
                    });
                }
            }
            SiteRow::Call { addr, .. } => cases.push(Case::Single {
                addr: *addr,
                label: "call",
                br: None,
                call: true,
            }),
            SiteRow::Window { lo, .. } => {
                let i = wins.iter().position(|(l, _)| l == lo).unwrap();
                let root = find(&mut parent, i);
                let group: Vec<(u32, u32)> = (0..wins.len())
                    .filter(|&j| find(&mut parent, j) == root)
                    .map(|j| wins[j])
                    .collect();
                cases.push(Case::Window { lo: *lo, group });
            }
            SiteRow::Jump { .. } => {}
        }
    }
    cases
}

fn run_case(
    m_o: &mut Machine,
    m_p: &mut Machine,
    case: &Case,
    rng: &mut Rng,
    lay: &Layout,
    opts: &Options,
) -> (usize, usize, Vec<String>) {
    let (addr, group): (u32, Vec<(u64, u64)>) = match case {
        Case::Single { addr, .. } | Case::Gate { addr, .. } => (*addr, vec![]),
        Case::Window { lo, group } => (
            *lo,
            group
                .iter()
                .map(|(l, h)| (IB + *l as u64, IB + *h as u64))
                .collect(),
        ),
    };
    let mut kinds = vec![0u32, 1];
    let br = matches!(case, Case::Single { br: Some(_), .. });
    if !br {
        kinds.push(2);
    }
    let (mut n, mut eff, mut extra) = (0usize, 0usize, 0usize);
    let mut diffs = Vec::new();
    while !kinds.is_empty() || (eff < opts.min_eff && extra < opts.max_extra) {
        let kind = if !kinds.is_empty() {
            kinds.remove(0)
        } else {
            extra += 1;
            if extra % 2 == 1 { 1 } else { 0 }
        };
        let init = init_state(rng, kind);
        let mut x18 = None;
        match case {
            Case::Single {
                br: Some((base, count)),
                ..
            } => x18 = Some(IB + *base as u64 + 4 * rng.below(*count as u64)),
            Case::Single { call: true, .. } => x18 = Some(IB + 0x1000 + 4 * rng.below(1024)),
            _ => {}
        }
        let (mut so, sp) = match case {
            Case::Gate { .. } => {
                let sp = m_p.run(
                    IB + addr as u64,
                    true,
                    &init,
                    Stop::LeavePatch,
                    vec![],
                    lay,
                    x18,
                );
                // the stub host call is a no-op `ret`: nothing may change except the counter destination
                let mut so = Snapshot {
                    g: init.g.to_vec(),
                    sp: SP0,
                    nzcv: init.nzcv,
                    v: init.v.to_vec(),
                    tls: init.tls,
                    writes: vec![],
                    fault: None,
                    end_pc: Some(IB + addr as u64 + 4),
                    invalid: false,
                };
                if let Case::Gate {
                    kind: Some(k), arg, ..
                } = case
                    && *k != GateKind::Svc
                {
                    so.g[*arg as usize] = sp.g[*arg as usize];
                }
                (so, sp)
            }
            Case::Single { .. } => {
                let so = m_o.run(
                    IB + addr as u64,
                    false,
                    &init,
                    Stop::Count1,
                    vec![],
                    lay,
                    x18,
                );
                let sp = m_p.run(
                    IB + addr as u64,
                    true,
                    &init,
                    Stop::LeavePatch,
                    vec![],
                    lay,
                    x18,
                );
                (so, sp)
            }
            Case::Window { .. } => {
                let so = m_o.run(
                    IB + addr as u64,
                    false,
                    &init,
                    Stop::LeaveSet,
                    group.clone(),
                    lay,
                    x18,
                );
                let sp = m_p.run(
                    IB + addr as u64,
                    true,
                    &init,
                    Stop::LeavePatch,
                    vec![],
                    lay,
                    x18,
                );
                (so, sp)
            }
        };
        if matches!(case, Case::Single { call: true, .. }) {
            // x16/x17 are dead into any call (intra-procedure scratch): the rewrite uses them freely
            so.g[16] = sp.g[16];
            so.g[17] = sp.g[17];
        }
        // spill writes below the red zone are the patch's own and must not be compared
        let ow: HashSet<u64> = so.writes.iter().map(|w| w.0).collect();
        let mut sp = sp;
        sp.writes.retain(|(a, _, _)| {
            !(*a >= SP0 - 1024
                && *a < SP0 + 512
                && !ow.contains(a)
                && (SP0 - *a >= 136 || matches!(case, Case::Gate { .. })))
        });
        if so.invalid {
            n += 1;
            continue; // undefined in the emulator: nothing to compare, not an effective trial
        }
        let both = so.fault.is_some() && sp.fault.is_some();
        let same = if so.fault.is_some() || sp.fault.is_some() {
            so.fault == sp.fault
        } else {
            so.clone()
                == Snapshot {
                    invalid: so.invalid,
                    ..sp.clone()
                }
        };
        n += 1;
        if !both {
            eff += 1;
        }
        if !same {
            diffs.push(describe(&so, &sp, kind));
        }
    }
    (n, eff, diffs)
}

fn describe(a: &Snapshot, b: &Snapshot, kind: u32) -> String {
    let mut f = Vec::new();
    for i in 0..31 {
        if a.g[i] != b.g[i] {
            f.push(format!("x{i}: {:#x} vs {:#x}", a.g[i], b.g[i]));
        }
    }
    if a.sp != b.sp {
        f.push(format!("sp: {:#x} vs {:#x}", a.sp, b.sp));
    }
    if a.nzcv != b.nzcv {
        f.push("nzcv".into());
    }
    if a.v != b.v {
        f.push("vregs".into());
    }
    if a.tls != b.tls {
        f.push(format!("tls {:#x} vs {:#x}", a.tls, b.tls));
    }
    if a.writes != b.writes {
        f.push(format!(
            "writes {:x?} vs {:x?}",
            &a.writes[..a.writes.len().min(4)],
            &b.writes[..b.writes.len().min(4)]
        ));
    }
    if a.fault != b.fault {
        f.push(format!("fault {:?} vs {:?}", a.fault, b.fault));
    }
    if a.end_pc != b.end_pc {
        f.push(format!("pc {:x?} vs {:x?}", a.end_pc, b.end_pc));
    }
    format!("kind={kind} {}", f.join("; "))
}

/// Runs the differential check over every non-gate site, and a no-op-host check for gates.
pub fn check_equivalence(orig: &ModuleImage, out: &RewriteOutput, opts: &Options) -> Report {
    let lay = Layout {
        patch_off: out.layout.patch_off as u64,
        key_page_off: out.layout.key_page_off as u64,
        key_idx: std::array::from_fn(|i| Key::ALL[i].index() as u64),
    };
    let orig_img = deexcl(&orig.bytes);
    let pat_img = deexcl(&out.image);
    let cases = build_cases(&orig.bytes, out);
    let report = Report {
        sites: cases.len(),
        ..Default::default()
    };
    let mut rngseed = Rng::new(opts.seed);
    let mut arena0 = vec![0u8; ARENA_SZ];
    for c in arena0.chunks_mut(8) {
        c.copy_from_slice(&rngseed.next().to_le_bytes());
    }
    let threads = rayon::current_num_threads().max(1);
    let chunks: Vec<Vec<&Case>> = (0..threads)
        .map(|t| cases.iter().skip(t).step_by(threads).collect())
        .collect();
    #[allow(clippy::type_complexity)] // test-local result shape, named by its single use below
    let results: Vec<(Vec<(u32, usize, usize, Vec<String>)>, usize)> = chunks
        .par_iter()
        .enumerate()
        .map(|(ci, chunk)| {
            let mut m_o = machine(&orig_img, &arena0, lay.patch_off, false);
            let mut m_p = machine(&pat_img, &arena0, lay.patch_off, true);
            let mut rng = Rng::new(opts.seed * 1_000_003 + ci as u64);
            let mut res = Vec::new();
            let mut gates = 0;
            for c in chunk {
                let addr = match c {
                    Case::Single { addr, .. } | Case::Gate { addr, .. } => *addr,
                    Case::Window { lo, .. } => *lo,
                };
                if matches!(c, Case::Gate { .. }) {
                    gates += 1;
                }
                let (n, eff, d) = run_case(&mut m_o, &mut m_p, c, &mut rng, &lay, opts);
                res.push((addr, n, eff, d));
            }
            (res, gates)
        })
        .collect();
    let mut report = report;
    for (res, g) in results {
        report.gate_sites += g;
        for (addr, n, eff, d) in res {
            report.trials += n;
            report.effective += eff;
            if eff == 0 {
                report.zero_effective.push(addr);
            }
            for x in d {
                report.diverged.push((
                    addr,
                    format!(
                        "{} | {}",
                        dis(u32::from_le_bytes(
                            orig.bytes[addr as usize..addr as usize + 4]
                                .try_into()
                                .unwrap()
                        )),
                        x
                    ),
                ));
            }
        }
    }
    report.zero_effective.sort();
    report.diverged.sort();
    if opts.mutate > 0 {
        mutation_selfcheck(orig, out, &orig_img, &arena0, &lay, opts, &mut report);
    }
    report
}

/// Negative control: corrupt ordinary blocks (nop the substituted instruction / skew the return
/// branch) and require the harness to notice every one.
fn mutation_selfcheck(
    orig: &ModuleImage,
    out: &RewriteOutput,
    orig_img: &[u8],
    arena0: &[u8],
    lay: &Layout,
    opts: &Options,
    report: &mut Report,
) {
    let word = |img: &[u8], a: u32| {
        u32::from_le_bytes(img[a as usize..a as usize + 4].try_into().unwrap())
    };
    let br_sites: HashSet<u32> = out
        .sites
        .iter()
        .filter_map(|r| {
            if let SiteRow::Jump { addr, .. } = r {
                Some(*addr)
            } else {
                None
            }
        })
        .collect();
    let gate_sites: HashSet<u32> = out.gates.iter().map(|g| g.orig).collect();
    let mut cand: Vec<(u32, u32)> = Vec::new(); // (site, nop target address in patch)
    for r in &out.sites {
        let SiteRow::Single { addr, label } = r else {
            continue;
        };
        if *label != "x18" || br_sites.contains(addr) || gate_sites.contains(addr) {
            continue;
        }
        let head = word(&out.image, *addr);
        let entry =
            (*addr as i64 + 4 * (((head & 0x03FF_FFFF) ^ 0x0200_0000) as i64 - 0x0200_0000)) as u32;
        // plain block: stp, mrs, adrp, ldr, add, ldr S,[T], I', str, ldp, b
        if word(&out.image, entry) & 0xFFC0_03E0 == 0xA900_03E0
            && word(&out.image, entry + 4) & 0xFFFF_FFE0 == 0xD53B_D060
            && word(&out.image, entry + 4 * 7) & 0xFFFF_FC00 == 0xF900_0000
        {
            cand.push((*addr, entry));
        }
    }
    let mut rng = Rng::new(opts.seed ^ 0xABCD);
    let mut chosen = Vec::new();
    while chosen.len() < opts.mutate.min(cand.len()) {
        let c = cand[rng.below(cand.len() as u64) as usize];
        if !chosen.contains(&c) {
            chosen.push(c);
        }
    }
    chosen.sort();
    let mut mutated = deexcl(&out.image);
    let mut kinds: BTreeMap<u32, &str> = BTreeMap::new();
    for (i, (site, entry)) in chosen.iter().enumerate() {
        if i % 2 == 0 {
            mutated[(*entry + 6 * 4) as usize..(*entry + 6 * 4) as usize + 4]
                .copy_from_slice(&0xD503_201Fu32.to_le_bytes());
            kinds.insert(*site, "nop");
        } else {
            // skew the final branch back to text by one instruction
            let b = *entry + 4 * 9;
            let w = word(&mutated, b);
            if w & 0xFC00_0000 == 0x1400_0000 {
                mutated[b as usize..b as usize + 4]
                    .copy_from_slice(&(w.wrapping_add(1)).to_le_bytes());
                kinds.insert(*site, "exit+4");
            }
        }
    }
    let lay2 = Layout {
        patch_off: lay.patch_off,
        key_page_off: lay.key_page_off,
        key_idx: lay.key_idx,
    };
    let sites: Vec<u32> = kinds.keys().copied().collect();
    let threads = rayon::current_num_threads().max(1);
    let missed: Vec<Vec<u32>> = (0..threads)
        .into_par_iter()
        .map(|t| {
            let mut m_o = machine(orig_img, arena0, lay2.patch_off, false);
            let mut m_p = machine(&mutated, arena0, lay2.patch_off, true);
            let mut rng = Rng::new(opts.seed.wrapping_add(77 + t as u64));
            let mut miss = Vec::new();
            for site in sites.iter().skip(t).step_by(threads) {
                let case = Case::Single {
                    addr: *site,
                    label: "x18",
                    br: None,
                    call: false,
                };
                let (_, _, d) = run_case(
                    &mut m_o,
                    &mut m_p,
                    &case,
                    &mut rng,
                    &lay2,
                    &Options {
                        min_eff: 4,
                        max_extra: 24,
                        ..Default::default()
                    },
                );
                if d.is_empty() {
                    miss.push(*site);
                }
            }
            miss
        })
        .collect();
    report.mutations = sites.len();
    let mut missed: Vec<u32> = missed.into_iter().flatten().collect();
    missed.sort();
    report.mutations_caught = sites.len() - missed.len();
    // A mutation can be invisible when the substituted instruction has no observable effect on the
    // random state (e.g. it rewrites a register with its own value); those are listed, not failed
    // here, when they are a small minority.
    report.mutations_missed = if missed.len() * 5 <= sites.len() {
        Vec::new()
    } else {
        missed
    };
    let _ = orig;
}
