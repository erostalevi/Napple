//! Seeded-mutation robustness tests for the NSO-side parsers of napple-rewrite: module layout, MOD0,
//! the dynamic section, RELR/RELA/JMPREL decoding, `.eh_frame_hdr` function starts and the whole
//! `rewrite_module` pipeline over hostile text. Errors are fine; panics, hangs and huge allocations
//! are not. Scale with NAPPLE_MUT_ITERS / NAPPLE_MUT_SEED.
#[allow(dead_code)]
#[path = "../../napple-core/tests/mutlib/mod.rs"]
mod mutlib;

use mutlib::*;
use napple_core::infrastructure::nso::{Nso, Segment};
use napple_rewrite::image::ModuleImage;
use napple_rewrite::survey::survey_module;
use napple_rewrite::{RewriteOptions, relocs, rewrite_module};

fn put32(v: &mut [u8], o: usize, x: u32) {
    v[o..o + 4].copy_from_slice(&x.to_le_bytes());
}
fn put64(v: &mut [u8], o: usize, x: u64) {
    v[o..o + 8].copy_from_slice(&x.to_le_bytes());
}

/// A module with MOD0, a dynamic section (RELR + RELA + JMPREL + HASH + SYMTAB), an eh_frame_hdr and a
/// text made of the instruction shapes the rewriter cares about (mrs/msr x18 users, ldxr windows, br, bl, svc).
fn seed_nso() -> Nso {
    let mut r = Rng(0x1234_5678_9abc_def1);
    let mut text = vec![0u8; 0x1000];
    let shapes: [u32; 14] = [
        0xD503_201F, // nop
        0xD65F_03C0, // ret
        0x9400_0010, // bl +0x40
        0x1400_0020, // b
        0xD53B_D040, // mrs x0, tpidrro_el0
        0xD53B_D041, // mrs x1, tpidr_el0
        0xD4000001,  // svc #0
        0xC85F_7C00, // ldxr x0,[x0]
        0xC801_7C00, // stxr w1,x0,[x0]
        0xD61F_0240, // br x18
        0xF940_0240, // ldr x0,[x18]
        0xD000_0000, // adrp x0
        0xD63F_0200, // blr x16
        0xAA12_03E0, // mov x0,x18
    ];
    for i in 0..0x400 {
        let w = if r.below(5) == 0 {
            r.next() as u32
        } else {
            shapes[r.below(shapes.len())]
        };
        put32(&mut text, i * 4, w);
    }
    // MOD0 at 0x8: dynamic at 0x2000 (data), eh_frame_hdr at 0x1000 (rodata)
    put32(&mut text, 4, 8);
    text[8..12].copy_from_slice(b"MOD0");
    put32(&mut text, 12, (0x2000i32 - 8) as u32);
    put32(&mut text, 24, (0x1000i32 - 8) as u32);
    let mut ro = vec![0u8; 0x1000];
    // eh_frame_hdr: version 1, ptr enc 0x1b?, we use udata4 (0x03) absolute so values are plain
    ro[0] = 1;
    ro[1] = 0x03;
    ro[2] = 0x03;
    ro[3] = 0x03;
    put32(&mut ro, 4, 0);
    put32(&mut ro, 8, 4);
    for i in 0..4u32 {
        put32(&mut ro, 12 + i as usize * 8, 0x40 * i);
        put32(&mut ro, 16 + i as usize * 8, 0x1000 + 0x100 * i);
    }
    // HASH at 0x1100 (nsyms), SYMTAB at 0x1200 (3 symbols)
    put32(&mut ro, 0x100 + 4, 3);
    put32(&mut ro, 0x200 + 24 + 6, 1);
    put64(&mut ro, 0x200 + 24 + 8, 0x40);
    let mut data = vec![0u8; 0x1000];
    let tags: [(i64, u64); 10] = [
        (36, 0x2100),
        (35, 16),
        (37, 8),
        (7, 0x2200),
        (8, 48),
        (23, 0x2300),
        (2, 24),
        (4, 0x1100),
        (6, 0x1200),
        (0, 0),
    ];
    for (i, (t, v)) in tags.iter().enumerate() {
        put64(&mut data, i * 16, *t as u64);
        put64(&mut data, i * 16 + 8, *v);
    }
    put64(&mut data, 0x100, 0x2400);
    put64(&mut data, 0x108, (1 << 1) | 1);
    put64(&mut data, 0x400, 0x40);
    put64(&mut data, 0x408, 0x80);
    put64(&mut data, 0x200, 0x2500);
    put64(&mut data, 0x208, 1027);
    put64(&mut data, 0x210, 0x40);
    put64(&mut data, 0x218, 0x2508);
    put64(&mut data, 0x220, (1 << 32) | 1025);
    put64(&mut data, 0x228, 0);
    Nso {
        build_id: [0x5a; 32],
        text: Segment {
            mem_offset: 0,
            data: text,
        },
        rodata: Segment {
            mem_offset: 0x1000,
            data: ro,
        },
        data: Segment {
            mem_offset: 0x2000,
            data,
        },
        bss_size: 0x1000,
    }
}

fn mutate_nso(n: &Nso, r: &mut Rng) -> Nso {
    let mut n = n.clone();
    for _ in 0..1 + r.below(4) {
        match r.below(8) {
            0 => n.bss_size = HUGE[r.below(HUGE.len())] as u32,
            1 => n.text.mem_offset = HUGE[r.below(HUGE.len())] as u32,
            2 => n.rodata.mem_offset = HUGE[r.below(HUGE.len())] as u32,
            3 => n.data.mem_offset = HUGE[r.below(HUGE.len())] as u32,
            4 => n.text.data.truncate(r.below(n.text.data.len() + 1)),
            _ => {
                let seg = match r.below(3) {
                    0 => &mut n.text.data,
                    1 => &mut n.rodata.data,
                    _ => &mut n.data.data,
                };
                *seg = mutate(seg, seg.len(), r);
            }
        }
    }
    n
}

fn exercise(n: &Nso) {
    let Ok(img) = ModuleImage::from_nso(n) else {
        return;
    };
    let tags = img.dynamic_tags();
    let _ = img.function_starts();
    let _ = relocs::collect(
        &img.bytes,
        &tags,
        n.data.mem_offset as u64,
        img.layout.image_len as u64,
    );
    let _ = relocs::decode_relr(&img.bytes, &tags, 0, img.layout.image_len as u64);
    let _ = rewrite_module(n, "00", &RewriteOptions::default());
}

#[test]
fn seed_module_is_well_formed() {
    let n = seed_nso();
    let img = ModuleImage::from_nso(&n).unwrap();
    assert!(img.mod0.is_some());
    assert_eq!(img.dynamic_tags().len(), 9);
    assert_eq!(img.function_starts().len(), 4);
    let r = relocs::collect(
        &img.bytes,
        &img.dynamic_tags(),
        0x2000,
        img.layout.image_len as u64,
    )
    .unwrap();
    assert!(r.len() >= 4, "{r:?}");
    assert!(rewrite_module(&n, "00", &RewriteOptions::default()).is_ok());
}

#[test]
fn mutate_module_and_rewrite() {
    let seed = seed_nso();
    let mut r = Rng(0xfeed_f00d_1234_5679);
    for i in 0..iters() {
        let m = mutate_nso(&seed, &mut r);
        let t = std::time::Instant::now();
        bigalloc::reset();
        exercise(&m);
        assert!(
            bigalloc::max() <= ALLOC_LIMIT,
            "case {i}: allocated {} at once",
            bigalloc::max()
        );
        assert!(t.elapsed().as_secs() < 20, "case {i} too slow");
    }
}

#[test]
fn mutate_survey_bytes() {
    // file-level: survey_module parses NSO bytes itself
    let n = seed_nso();
    let mut f = vec![0u8; 0x100];
    f[..4].copy_from_slice(b"NSO0");
    let segs = [&n.text.data, &n.rodata.data, &n.data.data];
    let mut body = Vec::new();
    for (i, s) in segs.iter().enumerate() {
        let h = 0x10 + i * 0x10;
        put32(&mut f, h, (0x100 + body.len()) as u32);
        put32(&mut f, h + 4, (i * 0x1000) as u32);
        put32(&mut f, h + 8, s.len() as u32);
        put32(&mut f, 0x60 + i * 4, s.len() as u32);
        body.extend_from_slice(s);
    }
    f.extend(body);
    survey_module("seed", &f, false);
    run("survey", &[f], 0x100, |d| {
        survey_module("m", d, false);
    });
}

#[test]
fn hostile_dynamic_values_do_not_overflow() {
    let img_len = 0x3000u64;
    let img = vec![0u8; img_len as usize];
    let mk = |pairs: &[(i64, u64)]| {
        pairs
            .iter()
            .copied()
            .collect::<std::collections::BTreeMap<_, _>>()
    };
    // RELR table values near the top of the address space
    let mut b = vec![0u8; 0x400];
    b[0x200..0x208].copy_from_slice(&0xFFFF_FFFF_FFFF_FFF8u64.to_le_bytes());
    let t = mk(&[(36, 0x200), (35, 8), (37, 8)]);
    assert!(relocs::decode_relr(&b, &t, 0x100, 0x400).is_err());
    // bitmap after a near-max address
    b[0x200..0x208].copy_from_slice(&0x1000_0000_0000_0000u64.to_le_bytes());
    b[0x208..0x210].copy_from_slice(&u64::MAX.to_le_bytes());
    let t = mk(&[(36, 0x200), (35, 16), (37, 8)]);
    assert!(relocs::decode_relr(&b, &t, 0, 0x400).is_err());
    // RELA/JMPREL/HASH/SYMTAB pointing at the end of the address space
    for k in [u64::MAX, u64::MAX - 7, u64::MAX - 23, 1 << 63] {
        let t = mk(&[(7, k), (8, 48), (23, k), (2, 24), (4, k), (6, k)]);
        let _ = relocs::collect(&img, &t, 0, img_len);
    }
}
