//! Per-template tests: each site template is rewritten inside a small synthetic module and checked
//! for equivalence against the original instruction with Unicorn (random register states).
mod support;
use napple_core::infrastructure::nso::{Nso, Segment};
use napple_rewrite::a64;
use napple_rewrite::{rewrite_module, RewriteOptions, RewriteOutput};
use support::equiv::{check_equivalence, Options, Rng};
use support::synth;

const NOP: u32 = 0xD503_201F;

fn rewrite(nso: &Nso) -> (napple_rewrite::image::ModuleImage, RewriteOutput) {
    let out = rewrite_module(nso, "ab", &RewriteOptions::default()).unwrap();
    (napple_rewrite::image::ModuleImage::from_nso(nso).unwrap(), out)
}

/// Rewrites, requires a complete rewrite, then requires equivalence on all sites.
fn assert_equiv(nso: &Nso, expect_sites: usize) -> RewriteOutput {
    let (orig, out) = rewrite(nso);
    assert!(out.unhandled.is_empty(), "unhandled: {:?}", out.unhandled);
    let rep = check_equivalence(&orig, &out, &Options { mutate: 0, ..Default::default() });
    assert!(rep.diverged.is_empty(), "divergence: {:#?}", rep.diverged);
    assert!(rep.zero_effective.is_empty(), "no effective trial at {:x?}", rep.zero_effective);
    assert_eq!(rep.sites, expect_sites, "{rep:?}");
    napple_rewrite::check::assert_structure(&orig, &out);
    out
}

fn pad(code: &[u32]) -> Vec<u32> {
    // each instruction followed by nops so a site's fall-through is valid code
    code.iter().flat_map(|w| [*w, NOP, NOP]).collect()
}

#[test]
fn plain_forms_with_x18_operands() {
    let words = [
        0x9100_2241u32, // add x1, x18, #8
        0xF940_0A40,    // ldr x0, [x18, #16]
        0xF940_0412,    // ldr x18, [x0, #8]
        0xAA05_03F2,    // mov x18, x5
        0xAA12_03E5,    // mov x5, x18
        0x8B12_0A52,    // add x18, x18, x18, lsl #2
        0xA940_0A52,    // ldp x18, x2, [x18]
        0x3DC0_0640,    // ldr q0, [x18, #16]
        0xB872_6A40,    // ldr w0, [x18, x18]
        0xF9000_7F2 & 0xFFFF_FFFF, // placeholder replaced below
    ];
    let mut w = words.to_vec();
    w.pop();
    w.extend([
        0xF900_07F2, // str x18, [sp, #8]
        0xF940_0BF2, // ldr x18, [sp, #16]
        0xA9BF_4FF2, // stp x18, x19, [sp, #-16]!
        0xA8C1_4FF2, // ldp x18, x19, [sp], #16
        0xB81F_83F2, // stur w18, [sp, #-8]   (red zone)
        0x9100_03F2, // mov x18, sp
        0x9100_23F2, // add x18, sp, #8
        0x1A8F_B240, // csel w0, w18, w15, lt
        0xEB12_001F, // cmp x0, x18
        0xFA52_0004 | (18 << 5), // ccmp x0, x18, #4, eq (rn=x0..)
    ]);
    let nso = synth::nso(&pad(&w));
    let (orig, out) = rewrite(&nso);
    // every word above must name x18 and be handled as a plain site
    for (i, wd) in w.iter().enumerate() {
        assert!(a64::uses_reg18(*wd) == Some(true) || i == w.len() - 1, "{wd:08x} does not use x18: {:?}", a64::disasm(*wd));
    }
    assert!(out.unhandled.is_empty(), "{:?}", out.unhandled);
    let rep = check_equivalence(&orig, &out, &Options { mutate: 40, ..Default::default() });
    assert!(rep.ok(), "{rep:#?}");
    assert!(rep.sites >= w.len() - 1);
    napple_rewrite::check::assert_structure(&orig, &out);
}

#[test]
fn red_zone_is_preserved() {
    // leaf code that keeps live data at sp-16..sp and touches x18
    let nso = synth::nso(&pad(&[0xF81F_03F2 | (0 << 0) /* stur x18, [sp, #-16] */, 0xF85F_03F2 /* ldur x18, [sp, #-16] */]));
    let out = assert_equiv(&nso, 2);
    assert!(out.stats["plain"] == 2);
}

#[test]
fn conditional_branches_on_x18() {
    // cbz x18, +16 / cbnz w18, -8 / tbz w18, #3, +12 / tbnz x18, #40, +8
    let cbz = 0xB400_0000 | (4 << 5) | 18;
    let cbnz_w = 0x3500_0000 | (((-2i32) as u32 & 0x7FFFF) << 5) | 18;
    let tbz = 0x3600_0000 | (3 << 19) | (3 << 5) | 18;
    let tbnz_x = 0x3700_0000 | (1u32 << 31) | (8 << 19) | (2 << 5) | 18;
    let nso = synth::nso(&pad(&[cbz, cbnz_w, tbz, tbnz_x]));
    let out = assert_equiv(&nso, 4);
    assert_eq!(out.stats["cond"], 4);
}

#[test]
fn pc_relative_x18() {
    let adr = 0x1000_0000 | (5 << 5) | 18; // adr x18, +0x14
    let adrp = 0x9000_0000 | (1 << 29) | (2 << 5) | 18; // adrp x18, ...
    let nso = synth::nso(&pad(&[adr, adrp]));
    let out = assert_equiv(&nso, 2);
    assert_eq!(out.stats["pcrel"], 2);
}

#[test]
fn literal_loads_into_x18() {
    // ldr x18, lit ; ldr w18, lit ; ldrsw x18, lit ; literal words follow, then nops
    let ldr_x = 0x5800_0000 | (5 << 5) | 18;
    let ldr_w = 0x1800_0000 | (4 << 5) | 18;
    let ldrsw = 0x9800_0000 | (3 << 5) | 18;
    let code = [ldr_x, ldr_w, ldrsw, NOP, NOP, 0xCAFE_BABE, 0x8000_0001, 0x1234_5678, 0x9ABC_DEF0, NOP, NOP];
    let nso = synth::nso(&code);
    let (orig, out) = rewrite(&nso);
    assert!(out.unhandled.is_empty(), "{:?}", out.unhandled);
    assert_eq!(out.stats["literal"], 3);
    let rep = check_equivalence(&orig, &out, &Options { mutate: 0, ..Default::default() });
    assert!(rep.ok(), "{rep:#?}");
}

#[test]
fn system_registers_and_host_gates() {
    let words = [
        0xD53B_D043, // mrs x3, tpidr_el0
        0xD51B_D044, // msr tpidr_el0, x4
        0xD53B_D067, // mrs x7, tpidrro_el0
        0xD53B_D050, // mrs x16, tpidr_el0
        0xD53B_D071, // mrs x17, tpidrro_el0
        0xD51B_D050, // msr tpidr_el0, x16
        0xD400_03A1, // svc #0x1d
        0xD53B_E025, // mrs x5, cntpct_el0
        0xD53B_E046, // mrs x6, cntvct_el0
        0xD53B_E007, // mrs x7, cntfrq_el0
    ];
    let nso = synth::nso(&pad(&words));
    let (orig, out) = rewrite(&nso);
    assert!(out.unhandled.is_empty(), "{:?}", out.unhandled);
    assert_eq!(out.gates.len(), 4);
    let rep = check_equivalence(&orig, &out, &Options { mutate: 0, ..Default::default() });
    assert!(rep.ok(), "{rep:#?}");
    assert_eq!(rep.gate_sites, 4);
    napple_rewrite::check::assert_structure(&orig, &out);
}

#[test]
fn br_x18_adr_byte_table() {
    // adr x18, base ; ldrb w9,[x8,x9] ; add x18,x18,x9,lsl #2 ; br x18 ; ... base: 256 nops
    let base = 0x40u32;
    let adr = 0x1000_0000 | (((base - 0) >> 2) << 5) | 18;
    let mut code = vec![adr, 0x3869_6909, 0x8B09_0A52, 0xD61F_0240];
    while code.len() < (base / 4) as usize {
        code.push(NOP);
    }
    code.extend(std::iter::repeat(NOP).take(256 + 4));
    let nso = synth::nso(&code);
    let out = assert_equiv(&nso, 3); // adr (pcrel), add (plain), br
    assert_eq!(out.stats["br"], 1);
    assert!(out.sites.iter().any(|r| matches!(r, napple_rewrite::SiteRow::Jump { base: b, count: 256, .. } if *b == base)));
}

#[test]
fn br_x18_signed_offset_table() {
    // adrp x17,page ; add x17,x17,#lo ; cmp w0,#3 ; b.hi out ; ldrsw x18,[x17,x0,lsl #2] ; add x18,x18,x17 ; br x18
    let text_len = 0x1000u32;
    let table = text_len; // ro starts right after text
    let targets = [0x40u32, 0x48, 0x50, 0x58];
    let code_prefix = |at: u32| -> Vec<u32> {
        let adrp = a64::adrp(17, at as u64, table as u64).unwrap();
        vec![adrp, a64::add_imm(17, 17, table & 0xFFF, false), 0x7100_0C1F /* cmp w0,#3 */, 0x5400_0000 | (8 << 5) | 8 /* b.hi +0x20 */, 0xB8A0_7A32 /* ldrsw x18,[x17,x0,lsl #2] */, 0x8B11_0252 /* add x18,x18,x17 */, 0xD61F_0240]
    };
    let mut code = code_prefix(0);
    while code.len() < 0x40 / 4 {
        code.push(NOP);
    }
    code.extend(std::iter::repeat(NOP).take(16));
    let mut nso = synth::nso(&code);
    let mut ro = Vec::new();
    for t in targets {
        ro.extend_from_slice(&((t as i64 - table as i64) as i32).to_le_bytes());
    }
    ro.resize(0x1000, 0);
    nso.rodata = Segment { mem_offset: nso.text.data.len() as u32, data: ro };
    nso.data.mem_offset = nso.text.data.len() as u32 + 0x1000;
    let out = assert_equiv(&nso, 3); // ldrsw + add x18 (plain) + br
    assert!(out.sites.iter().any(|r| matches!(r, napple_rewrite::SiteRow::Jump { base: 0x40, count: 7, .. })), "{:?}", out.sites);
}

#[test]
fn blr_x18_call() {
    let nso = synth::nso(&[0xD63F_0240, NOP, NOP, NOP]);
    let out = assert_equiv(&nso, 1);
    assert_eq!(out.stats["call_window"], 1);
}

#[test]
fn exclusive_windows() {
    // 1: ldxr x0,[x18] ; add x0,x0,#1 ; stxr w1,x0,[x18] ; cbnz w1, -12
    let ldxr = |rt: u32, rn: u32| 0xC85F_7C00u32 | (rn << 5) | rt;
    let stxr = |rs: u32, rt: u32, rn: u32| 0xC800_7C00u32 | (rs << 16) | (rn << 5) | rt;
    let cbnz = |rt: u32, off_words: i32| 0x3500_0000u32 | (((off_words as u32) & 0x7FFFF) << 5) | rt;
    let w1 = vec![ldxr(0, 18), 0x9100_0400, stxr(1, 0, 18), cbnz(1, -3), NOP, NOP, NOP, NOP];
    // 2: status register is w18: ldxr x17,[x12] ; cmp x17,x16 ; b.ne +16 ; and x18,x18,x11 ; stxr w18,x16,[x12] ; cbz w18, +8
    let w2 = vec![ldxr(17, 12), 0xEB10_023F, 0x5400_0081, 0x8A0B_0252, stxr(18, 16, 12), 0x3400_0000 | (2 << 5) | 18, NOP, NOP, NOP, NOP, NOP, NOP];
    // 3: ldaxr w5 / stlxr form with x18 base and two exits
    let w3 = vec![0x885F_FC00 | (18 << 5) | 5, 0x6B1F_00BF, 0x5400_0061, 0x8800_FC00 | (6 << 16) | (18 << 5) | 5, 0x3500_0000 | (((-4i32 as u32) & 0x7FFFF) << 5) | 6, NOP, NOP, NOP, NOP];
    let code: Vec<u32> = [w1, w2, w3].concat();
    let nso = synth::nso(&code);
    let (orig, out) = rewrite(&nso);
    assert!(out.unhandled.is_empty(), "{:?}", out.unhandled);
    assert_eq!(out.stats["window"], 3);
    let rep = check_equivalence(&orig, &out, &Options { mutate: 0, ..Default::default() });
    assert!(rep.ok(), "{rep:#?}");
    napple_rewrite::check::assert_structure(&orig, &out);
}

#[test]
fn call_inside_exclusive_window_is_refused() {
    // ldxr x0,[x18] ; bl +0x40 ; stxr w1,x0,[x18] ; cbnz w1,-12: the callee would return into the window copy with
    // the scratch registers already restored, so the window must be refused, not relocated
    let bl = 0x9400_0000u32 | 0x10;
    let mut code = vec![0xC85F_7C00u32 | (18 << 5), bl, 0xC800_7C00 | (1 << 16) | (18 << 5), 0x3500_0000 | (((-3i32 as u32) & 0x7FFFF) << 5) | 1];
    code.extend(std::iter::repeat(NOP).take(20));
    let nso = synth::nso(&code);
    let (_, out) = rewrite(&nso);
    assert!(!out.unhandled.is_empty(), "a bl inside an exclusive window must not be rewritten");
    assert!(out.unhandled.iter().any(|u| u.reason.contains("cannot be relocated into a window")), "{:?}", out.unhandled);
}

#[test]
fn x18_inside_unrecognised_exclusive_loop_is_refused() {
    // ldxr ... (17 instructions without a store-exclusive) ... then an x18 site
    let mut code = vec![0xC85F_7D80u32];
    code.extend(std::iter::repeat(NOP).take(18));
    code.push(0xAA00_03F2); // mov x18, x0
    let nso = synth::nso(&code);
    let (_, out) = rewrite(&nso);
    assert_eq!(out.unhandled.len(), 1, "{:?}", out.unhandled);
    assert!(out.unhandled[0].reason.contains("ldxr/stxr"));
}

#[test]
fn random_x18_instructions_match_the_original() {
    // Seeded corpus of random words forced to name x18 in 1-3 register fields.
    let mut rng = Rng::new(7);
    let mut words = Vec::new();
    while words.len() < 6000 {
        let mut w = rng.next() as u32;
        let top = w >> 24;
        if (0xD4..=0xD7).contains(&top) {
            continue;
        }
        for p in [0u32, 5, 10, 16] {
            if rng.below(2) == 0 || p == 5 {
                w = (w & !(31 << p)) | (18 << p);
            }
        }
        if a64::uses_reg18(w) != Some(true) || a64::is_control_flow(w) || a64::is_adr_like(w) || a64::is_literal_load(w) || a64::is_excl_load(w) || a64::is_excl_store(w) {
            continue;
        }
        if a64::disasm(w).is_some_and(|t| t.full.contains("invalid")) {
            continue;
        }
        if a64::disasm(w).is_some_and(|t| t.mnemonic.contains("g") && (t.mnemonic.starts_with("st") || t.mnemonic.starts_with("ld"))) {
            continue; // MTE tag instructions: not modelled by the emulator
        }
        words.push(w);
    }
    let nso = synth::nso(&pad(&words));
    let (orig, out) = rewrite(&nso);
    // anything left over must be an sp form the templates refuse for a stated reason
    for u in &out.unhandled {
        assert!(u.reason.contains("sp") || u.reason.contains("substitute"), "{u}");
    }
    let rep = check_equivalence(&orig, &out, &Options { mutate: 100, seed: 3, ..Default::default() });
    assert!(rep.diverged.is_empty(), "{:#?}", &rep.diverged[..rep.diverged.len().min(10)]);
    assert!(rep.mutations_missed.is_empty(), "{:?}", rep.mutations_missed.iter().map(|a| (*a, a64::disasm(orig.word(*a)).map(|t| t.full))).collect::<Vec<_>>());
    assert!(rep.sites > 3000, "only {} sites handled", rep.sites);
}
