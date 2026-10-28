//! Seeded-mutation robustness tests for the container-side parsers of napple-build (BKTR update
//! NCAs, NSP/XCI containers, the firmware zip reader, NRO headers, NTB1 tables, BFTTF fonts).
//! Same contract as napple-core/tests/mutation.rs: errors are fine, panics/hangs/huge allocations
//! are not. Scale with NAPPLE_MUT_ITERS / NAPPLE_MUT_SEED.
#[allow(dead_code)]
#[path = "../../napple-core/tests/mutlib/mod.rs"]
mod mutlib;
#[allow(dead_code)]
#[path = "../../napple-core/tests/mutnca/mod.rs"]
mod mutnca;
mod support;

use mutlib::*;
use napple_build::bktr::{self, RawNca};
use napple_build::container::Container;
use napple_build::firmware::Firmware;
use napple_build::macho::layout_from_ntb1;
use napple_build::nro::parse_nro;
use napple_build::sysdata::decode_bfttf;
use napple_build::zipread::ZipArchive;
use napple_core::domain::keys::KeySet;
use napple_core::infrastructure::io::{BytesSource, Src, read_vec};
use napple_core::infrastructure::nca::TitleKeys;
use napple_core::infrastructure::pfs::parse_pfs0;
use std::sync::Arc;

fn bytes(b: &[u8]) -> Src {
    Arc::new(BytesSource(b.to_vec()))
}

fn touch(s: &Src) {
    let n = s.size().min(1 << 16) as usize;
    let _ = read_vec(&**s, 0, n);
    if s.size() > 64 {
        let mut b = [0u8; 64];
        let _ = s.read_at(s.size() - 64, &mut b);
        let mid = s.size() / 2;
        let _ = s.read_at(mid, &mut b);
    }
}

struct Seeds {
    keys: KeySet,
    hk: [u8; 32],
    ncas: Vec<Vec<u8>>,
    base: Vec<u8>,
    nsp: Vec<u8>,
}

fn seeds() -> Seeds {
    static N: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
    let t = support::tmp(&format!(
        "mut{}",
        N.fetch_add(1, std::sync::atomic::Ordering::SeqCst)
    ));
    let g = support::make_game(&t, true);
    let keys = KeySet::load(&g.keys).unwrap();
    let nsp = std::fs::read(&g.update_nsp).unwrap();
    let fs = parse_pfs0(bytes(&nsp)).unwrap();
    let ncas = fs
        .entries
        .iter()
        .filter(|e| e.name.ends_with(".nca"))
        .map(|e| fs.read(&e.name).unwrap())
        .collect();
    Seeds {
        hk: keys.get::<32>("header_key").unwrap(),
        keys,
        ncas,
        base: g.base_romfs.clone(),
        nsp,
    }
}

fn exercise_bktr(nca: &[u8], keys: &KeySet, base: &Src) {
    let Ok(raw) = RawNca::open(bytes(nca), keys, &TitleKeys::new()) else {
        return;
    };
    for sec in &raw.sections {
        if let Ok(s) = raw.ctr_section(sec) {
            touch(&s);
        }
        if let Ok(s) = raw.raw_section(sec) {
            touch(&s);
        }
        if let Ok(p) = bktr::open_patched(&raw, sec, base.clone()) {
            touch(&p.data);
            touch(&p.patch);
            let _ = p.delta_entries(0);
            let _ = p.delta_entries(0x1000);
        }
    }
}

#[test]
fn mutate_bktr_update_nca() {
    let s = seeds();
    let base = bytes(&s.base);
    let keys = &s.keys;
    run("bktr-body", &s.ncas, 0x4000, |d| {
        exercise_bktr(d, keys, &base)
    });
    run_with(
        "bktr-header",
        &s.ncas,
        &|n, r| mutnca::mutate_nca_bytes(n, s.hk, r),
        |d| exercise_bktr(d, keys, &base),
    );
}

#[test]
fn mutate_bucket_trees() {
    let mut t = vec![0u8; 0x4000 * 2];
    t[4..8].copy_from_slice(&1u32.to_le_bytes());
    t[8..16].copy_from_slice(&0x500u64.to_le_bytes());
    t[0x4000 + 4..0x4000 + 8].copy_from_slice(&3u32.to_le_bytes());
    for i in 0..3u64 {
        let o = 0x4000 + 0x10 + i as usize * 0x14;
        t[o..o + 8].copy_from_slice(&(i * 0x100).to_le_bytes());
        t[o + 8..o + 16].copy_from_slice(&(i * 0x10).to_le_bytes());
    }
    run("bucket", &[t], 0x4100, |d| {
        for count in [0, 1, 3, 4, 1 << 20, u64::MAX] {
            let _ = bktr::parse_relocations(d, count);
            let _ = bktr::parse_subsections(d, count);
        }
    });
}

#[test]
fn mutate_container_files() {
    let s = seeds();
    let dir = std::env::temp_dir().join(format!("napple-bmut-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let keys = &s.keys;
    run("container", std::slice::from_ref(&s.nsp), 0x600, |d| {
        let p = dir.join("m.nsp");
        std::fs::write(&p, d).unwrap();
        let _ = Container::load_with_keys(&p, Some(keys));
    });
    let _ = std::fs::remove_dir_all(&dir);
}

fn zip_seed() -> Vec<u8> {
    use std::io::Write;
    let mut out = Vec::new();
    let mut cd = Vec::new();
    for (name, data, deflate) in [
        ("a.nca", vec![5u8; 300], false),
        ("dir/b.nca", vec![9u8; 4000], true),
    ] {
        let comp = if deflate {
            let mut e = flate2::write::DeflateEncoder::new(Vec::new(), flate2::Compression::fast());
            e.write_all(&data).unwrap();
            e.finish().unwrap()
        } else {
            data.clone()
        };
        let lho = out.len() as u32;
        let method = if deflate { 8u16 } else { 0 };
        out.extend([0x50, 0x4B, 3, 4, 20, 0, 0, 0]);
        out.extend(method.to_le_bytes());
        out.extend([0u8; 8]);
        out.extend((comp.len() as u32).to_le_bytes());
        out.extend((data.len() as u32).to_le_bytes());
        out.extend((name.len() as u16).to_le_bytes());
        out.extend([0, 0]);
        out.extend(name.as_bytes());
        out.extend(&comp);
        cd.extend([0x50, 0x4B, 1, 2, 20, 0, 20, 0, 0, 0]);
        cd.extend(method.to_le_bytes());
        cd.extend([0u8; 8]);
        cd.extend((comp.len() as u32).to_le_bytes());
        cd.extend((data.len() as u32).to_le_bytes());
        cd.extend((name.len() as u16).to_le_bytes());
        cd.extend([0u8; 12]);
        cd.extend(lho.to_le_bytes());
        cd.extend(name.as_bytes());
    }
    let cd_off = out.len() as u32;
    out.extend(&cd);
    out.extend([0x50, 0x4B, 5, 6, 0, 0, 0, 0]);
    out.extend(2u16.to_le_bytes());
    out.extend(2u16.to_le_bytes());
    out.extend((cd.len() as u32).to_le_bytes());
    out.extend(cd_off.to_le_bytes());
    out.extend([0, 0]);
    out
}

fn exercise_zip(d: &[u8]) {
    if let Ok(z) = ZipArchive::from_source(bytes(d)) {
        for e in &z.entries {
            if let Ok(s) = z.open_entry(e) {
                touch(&s);
            }
        }
    }
}

#[test]
fn mutate_firmware_zip() {
    let seed = zip_seed();
    exercise_zip(&seed);
    // the central directory sits at the end: bias there as well as to the local headers
    run("zip", std::slice::from_ref(&seed), seed.len(), exercise_zip);
    let dir = std::env::temp_dir().join(format!("napple-zmut-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    run("firmware", std::slice::from_ref(&seed), seed.len(), |d| {
        let p = dir.join("fw.zip");
        std::fs::write(&p, d).unwrap();
        let _ = Firmware::open(&p);
    });
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn zip_deflate_with_huge_declared_size_is_not_preallocated() {
    let mut z = zip_seed();
    // central directory entry 1 (deflate): claim 1 GiB of output for 40-odd compressed bytes
    let cd = z.windows(4).rposition(|w| w == [0x50, 0x4B, 1, 2]).unwrap();
    z[cd + 24..cd + 28].copy_from_slice(&((1u32 << 30) - 1).to_le_bytes());
    bigalloc::reset();
    exercise_zip(&z);
    assert!(
        bigalloc::max() <= ALLOC_LIMIT,
        "allocated {} bytes at once",
        bigalloc::max()
    );
}

fn nro_seed() -> Vec<u8> {
    let mut b = vec![0u8; 0x1000 * 3];
    b[0x10..0x14].copy_from_slice(b"NRO0");
    let n = b.len() as u32;
    b[0x18..0x1C].copy_from_slice(&n.to_le_bytes());
    for (o, v) in [
        (0x20, 0u32),
        (0x24, 0x1000),
        (0x28, 0x1000),
        (0x2C, 0x1000),
        (0x30, 0x2000),
        (0x34, 0x1000),
        (0x38, 0x1000),
    ] {
        b[o..o + 4].copy_from_slice(&v.to_le_bytes());
    }
    b
}

#[test]
fn mutate_nro_ntb1_bfttf() {
    run("nro", &[nro_seed()], 0x80, |d| {
        let _ = parse_nro(d);
    });
    let mut ntb = vec![0u8; 24 + 48];
    ntb[..4].copy_from_slice(b"NTB1");
    run("ntb1", &[ntb], 72, |d| {
        let _ = layout_from_ntb1(d);
    });
    let mut f = vec![0u8; 12 + 64];
    f[..4].copy_from_slice(&(0x1802_9a7fu32 ^ 0x0618_6249).to_le_bytes());
    run("bfttf", &[f], 76, |d| {
        let _ = decode_bfttf(d);
    });
}
