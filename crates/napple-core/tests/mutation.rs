//! Deterministic seeded-mutation tests for the untrusted-input parsers (the same entry points the
//! cargo-fuzz targets in fuzz/ drive). Seeds are the public synthetic fixtures; mutations are bit
//! flips, truncations, huge length fields, zeroed ranges and splices. A parser may return `Err`;
//! it must never panic, hang or allocate without bound. Scale with NAPPLE_MUT_ITERS / NAPPLE_MUT_SEED.
use napple_core::application::ingest::ingest;
use napple_core::domain::keys::KeySet;
use napple_core::infrastructure::cnmt::parse_cnmt;
use napple_core::infrastructure::io::{BytesSource, Src, read_vec};
use napple_core::infrastructure::ivfc::Ivfc;
use napple_core::infrastructure::nacp::parse_nacp;
use napple_core::infrastructure::nca::{self, FsType, Nca, TitleKeys};
use napple_core::infrastructure::ncz::NczSource;
use napple_core::infrastructure::nso::Nso;
use napple_core::infrastructure::pfs::{Fs, parse_hfs0, parse_pfs0};
use napple_core::infrastructure::romfs::RomFs;
use napple_core::infrastructure::ticket::{encrypted_title_key, parse_ticket};
use napple_core::infrastructure::xci::parse_xci;
use std::path::{Path, PathBuf};
use std::sync::Arc;
#[allow(dead_code)]
#[path = "mutlib/mod.rs"]
mod mutlib;
use mutlib::*;
#[allow(dead_code)]
#[path = "mutnca/mod.rs"]
mod mutnca;

fn fx(name: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/fixtures/nsz")
        .join(name)
}
fn read(name: &str) -> Vec<u8> {
    std::fs::read(fx(name)).unwrap()
}
fn keys() -> KeySet {
    KeySet::load(&fx("synthetic_keys.txt")).unwrap()
}
fn bytes(b: &[u8]) -> Src {
    Arc::new(BytesSource(b.to_vec()))
}

fn touch(fs: &Fs) {
    for e in fs.entries.clone() {
        if let Ok(s) = fs.open(&e.name) {
            let n = s.size().min(4096) as usize;
            let _ = read_vec(&*s, 0, n);
        }
    }
}

fn touch_src(s: &Src) {
    let n = s.size().min(1 << 16) as usize;
    let _ = read_vec(&**s, 0, n);
    if s.size() > 8 {
        let mut b = [0u8; 8];
        let _ = s.read_at(s.size() - 8, &mut b);
    }
}

fn touch_romfs(src: Src) {
    if let Ok(rf) = RomFs::parse(src) {
        for f in rf.files.iter().take(64) {
            if let Ok(s) = rf.open(f) {
                touch_src(&s);
            }
        }
    }
}

fn exercise_nca(src: Src) {
    let ks = keys();
    let Ok(n) = Nca::open(src, &ks, &TitleKeys::new()) else {
        return;
    };
    let _ = n.master_key_generation();
    for sec in &n.sections {
        let Ok(s) = n.section(sec.index) else {
            continue;
        };
        touch_src(&s);
        match sec.fs_type {
            FsType::Pfs0 => {
                if let Ok(d) = nca::pfs0_data(&s, sec)
                    && let Ok(fs) = parse_pfs0(d)
                {
                    touch(&fs);
                }
            }
            FsType::RomFs => {
                if let Ok(d) = nca::romfs_data(&s, sec) {
                    touch_romfs(d);
                }
                let _ = nca::romfs_data_verified(&s, sec);
            }
        }
    }
}

fn nsz_member(nsz: &str) -> Vec<u8> {
    let fs = parse_pfs0(bytes(&read(nsz))).unwrap();
    let name = fs
        .entries
        .iter()
        .find(|e| e.name.ends_with(".ncz"))
        .unwrap()
        .name
        .clone();
    fs.read(&name).unwrap()
}

#[test]
fn mutate_pfs0_hfs0_xci() {
    let seeds = vec![
        read("c00_block.nsz.bin")[..0x400].to_vec(),
        read("c00_solid.nsz.bin")[..0x400].to_vec(),
    ];
    run("pfs0", &seeds, 0x400, |d| {
        if let Ok(fs) = parse_pfs0(bytes(d)) {
            touch(&fs);
        }
    });
    run("hfs0", &seeds, 0x400, |d| {
        if let Ok(fs) = parse_hfs0(bytes(d)) {
            touch(&fs);
        }
    });
    run(
        "xci",
        &[read("c00.xcz.bin")[..0x20000].to_vec()],
        0x20000,
        |d| {
            if let Ok(parts) = parse_xci(bytes(d)) {
                for (_, fs) in &parts {
                    touch(fs);
                }
            }
        },
    );
}

#[test]
fn mutate_nca() {
    let seeds: Vec<Vec<u8>> = ["c00.nca.bin", "nca2.nca.bin", "g4000.nca.bin"]
        .iter()
        .map(|n| read(n))
        .collect();
    run("nca-body", &seeds, 0x4000, |d| exercise_nca(bytes(d)));
    run_with(
        "nca-header",
        &seeds,
        &|s, r| mutnca::mutate_nca_bytes(s, keys().get::<32>("header_key").unwrap(), r),
        |d| exercise_nca(bytes(d)),
    );
}

#[test]
fn mutate_ncz() {
    let seeds = vec![
        nsz_member("c00_block.nsz.bin"),
        nsz_member("c00_solid.nsz.bin"),
    ];
    run_with(
        "ncz-header",
        &seeds,
        &|s, r| mutnca::mutate_nca_bytes(s, keys().get::<32>("header_key").unwrap(), r),
        |d| {
            if let Ok(s) = NczSource::open(bytes(d)) {
                exercise_nca(s);
            }
        },
    );
    run("ncz", &seeds, 0x4100, |d| {
        if let Ok(s) = NczSource::open(bytes(d)) {
            touch_src(&s);
            exercise_nca(s);
        }
    });
}

#[test]
fn mutate_ingest_containers() {
    let seeds = vec![read("c00_block.nsz.bin"), read("c00.xcz.bin")];
    let dir = std::env::temp_dir().join(format!("napple-mut-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let ks = keys();
    run("ingest", &seeds, 0x1000, |d| {
        let p = dir.join("m.bin");
        std::fs::write(&p, d).unwrap();
        let _ = ingest(&p, &ks);
    });
    let _ = std::fs::remove_dir_all(&dir);
}

fn nso_seed() -> Vec<u8> {
    use sha2::{Digest, Sha256};
    let mut f = vec![0u8; 0x100];
    f[..4].copy_from_slice(b"NSO0");
    let segs: [&[u8]; 3] = [&[7; 600], b"ro-data", b"data"];
    let (mut flags, mut body) = (0u32, Vec::new());
    for (i, s) in segs.iter().enumerate() {
        let stored = lz4_flex::block::compress(s);
        flags |= (1 << i) | (8 << i);
        let h = 0x10 + i * 0x10;
        f[h..h + 4].copy_from_slice(&((0x100 + body.len()) as u32).to_le_bytes());
        f[h + 4..h + 8].copy_from_slice(&((i * 0x1000) as u32).to_le_bytes());
        f[h + 8..h + 12].copy_from_slice(&(s.len() as u32).to_le_bytes());
        f[0x60 + i * 4..0x64 + i * 4].copy_from_slice(&(stored.len() as u32).to_le_bytes());
        f[0xA0 + i * 0x20..0xC0 + i * 0x20].copy_from_slice(&Sha256::digest(s));
        body.extend(stored);
    }
    f[0xC..0x10].copy_from_slice(&flags.to_le_bytes());
    f.extend(body);
    f
}

#[test]
fn mutate_nso() {
    run("nso", &[nso_seed()], 0x100, |d| {
        let _ = Nso::parse(d);
    });
}

fn cnmt_seed() -> Vec<u8> {
    let mut d = vec![0u8; 0x20 + 0x10 + 0x38 * 3];
    d[8..12].copy_from_slice(&65536u32.to_le_bytes());
    d[0xC] = 0x81;
    d[0xE..0x10].copy_from_slice(&0x10u16.to_le_bytes());
    d[0x10..0x12].copy_from_slice(&3u16.to_le_bytes());
    d
}

#[test]
fn mutate_small_structures() {
    run("cnmt", &[cnmt_seed()], 0x40, |d| {
        let _ = parse_cnmt(d);
    });
    let mut nacp = vec![0u8; 0x4000];
    for (i, b) in nacp.iter_mut().enumerate().take(0x3000) {
        *b = if i % 0x300 < 12 { b'a' } else { 0 };
    }
    run("nacp", &[nacp], 0x4000, |d| {
        if let Ok(n) = parse_nacp(d) {
            let _ = (n.name(), n.publisher());
        }
    });
    let mut tik = vec![0u8; 4 + 0x23C + 0x2C0];
    tik[..4].copy_from_slice(&0x10004u32.to_le_bytes());
    tik[4..4 + 0x13C].fill(0);
    run("ticket", &[tik[..4 + 0x13C + 0x2C0].to_vec()], 0x500, |d| {
        if let Ok(t) = parse_ticket(d) {
            let _ = encrypted_title_key(&t, &keys());
        }
    });
}

#[test]
fn mutate_keyfile_text() {
    let seed = std::fs::read(fx("synthetic_keys.txt")).unwrap();
    run("keys", &[seed], 490, |d| {
        let _ = KeySet::parse(&String::from_utf8_lossy(d));
    });
}

#[test]
fn mutate_ivfc_romfs_tables() {
    let ks = keys();
    let n = Nca::open(bytes(&read("c00.nca.bin")), &ks, &TitleKeys::new()).unwrap();
    let mut seeds = Vec::new();
    let mut romfs_seeds = Vec::new();
    for sec in &n.sections {
        seeds.push(sec.hash_data.clone());
        if sec.fs_type == FsType::RomFs {
            let s = n.section(sec.index).unwrap();
            if let Ok(d) = nca::romfs_data(&s, sec) {
                romfs_seeds.push(read_vec(&*d, 0, d.size().min(1 << 16) as usize).unwrap());
            }
        }
    }
    run("ivfc", &seeds, 0x100, |d| {
        let _ = Ivfc::parse(d);
    });
    if !romfs_seeds.is_empty() {
        run("romfs", &romfs_seeds, 0x1000, |d| touch_romfs(bytes(d)));
    }
}

fn assert_small_alloc(what: &str, f: impl FnOnce()) {
    bigalloc::reset();
    f();
    assert!(
        bigalloc::max() <= ALLOC_LIMIT,
        "{what}: allocated {} bytes at once",
        bigalloc::max()
    );
}

#[test]
fn nso_huge_decompressed_size_is_not_preallocated() {
    // A compressed segment claiming 512 MiB of output from a few bytes (LZ4 expands at most ~255x).
    let mut f = nso_seed();
    for i in 0..3 {
        f[0x10 + i * 0x10 + 8..0x10 + i * 0x10 + 12].copy_from_slice(&(1u32 << 29).to_le_bytes());
    }
    assert_small_alloc("nso", || assert!(Nso::parse(&f).is_err()));
}

#[test]
fn ncz_block_with_huge_declared_size_is_not_preallocated() {
    // Block form with a 4 GiB block size and one tiny compressed block.
    let mut d = vec![0u8; 0x4000];
    d.extend_from_slice(b"NCZSECTN");
    d.extend_from_slice(&1u64.to_le_bytes());
    let mut sec = [0u8; 0x40];
    sec[..8].copy_from_slice(&0x4000u64.to_le_bytes());
    sec[8..16].copy_from_slice(&(1u64 << 32).to_le_bytes());
    sec[16..24].copy_from_slice(&3u64.to_le_bytes());
    d.extend_from_slice(&sec);
    let mut hdr = [0u8; 0x18];
    hdr[..8].copy_from_slice(b"NCZBLOCK");
    hdr[0x08] = 2;
    hdr[0x0b] = 32;
    hdr[0x0c..0x10].copy_from_slice(&1u32.to_le_bytes());
    hdr[0x10..0x18].copy_from_slice(&(1u64 << 32).to_le_bytes());
    d.extend_from_slice(&hdr);
    let comp = [0x28u8, 0xb5, 0x2f, 0xfd, 0x00, 0x00, 0x00];
    d.extend_from_slice(&(comp.len() as u32).to_le_bytes());
    d.extend_from_slice(&comp);
    assert_small_alloc("ncz", || {
        if let Ok(s) = NczSource::open(bytes(&d)) {
            let mut b = [0u8; 16];
            let _ = s.read_at(0x4000, &mut b);
        }
    });
}

/// Fuzzer finding (nca target): an NCA0 whose section starts inside the header (< 0x400) underflowed the
/// XTS sector number. File = the crash input with its header re-encrypted by the synthetic header key.
#[test]
fn regress_nca0_section_below_header() {
    let d = std::fs::read(
        Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/regress/nca0_section_below_header.bin"),
    )
    .unwrap();
    exercise_nca(bytes(&d));
}
