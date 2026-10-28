//! `Container::load` on files written by the public `nsz` tool (fixtures live in napple-core, see
//! its tests/fixtures/README.md): NSZ, XCZ, and a personalized ticket resolved with the eticket
//! device key. Synthetic keys only.
use napple_build::container::{Container, Containers};
use napple_core::domain::error::Error;
use napple_core::domain::keys::KeySet;
use napple_core::infrastructure::nca::{self, Nca};
use napple_core::infrastructure::pfs::parse_pfs0;
use std::path::{Path, PathBuf};

#[allow(dead_code)]
#[path = "../../napple-core/tests/nca0_fixture/mod.rs"]
mod nca0_fixture;

fn fx(name: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../napple-core/tests/fixtures/nsz")
        .join(name)
}

fn unhex(s: &str) -> Vec<u8> {
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
        .collect()
}

fn keys(with_device: bool) -> KeySet {
    let mut text = std::fs::read_to_string(fx("synthetic_keys.txt")).unwrap();
    if with_device {
        // eticket_rsa_keypair: D at 0x00, N at 0x100, E at 0x200 (big-endian)
        let mut kp = unhex(nca0_fixture::D);
        kp.extend(unhex(nca0_fixture::N));
        kp.extend([0, 1, 0, 1]);
        kp.resize(0x230, 0);
        let hex: String = kp.iter().map(|b| format!("{b:02x}")).collect();
        text.push_str(&format!("eticket_rsa_keypair = {hex}\n"));
    }
    KeySet::parse(&text).unwrap()
}

/// Opens the container's only NCA and returns the length of ExeFS `main`.
fn main_len(c: &Container, ks: &KeySet) -> napple_core::domain::error::Result<usize> {
    let (_, src) = c.ncas.iter().next().expect("one NCA");
    let n = Nca::open(src.clone(), ks, &c.tks)?;
    let sec = &n.sections[0];
    let fs = parse_pfs0(nca::pfs0_data(&n.section(sec.index)?, sec)?)?;
    Ok(fs.read("main")?.len())
}

#[test]
fn nsz_and_xcz_members_appear_as_ncas() {
    let ks = keys(false);
    for f in [
        "c00_solid.nsz.bin",
        "c00_block.nsz.bin",
        "g4000_solid.nsz.bin",
        "g4000_block.nsz.bin",
        "c00.xcz.bin",
    ] {
        let c = Container::load(&fx(f)).unwrap();
        assert_eq!(c.ncas.len(), 1, "{f}");
        assert!(c.ncas.keys().all(|n| n.ends_with(".nca")), "{f}");
        assert!(c.tks.is_empty(), "{f}");
        assert_eq!(main_len(&c, &ks).unwrap(), 150_000, "{f}");
    }
}

#[test]
fn personalized_ticket_through_container_load() {
    let ks = keys(true);
    let c = Container::load_with_keys(&fx("rights_solid.nsz.bin"), Some(&ks)).unwrap();
    assert_eq!(c.tks.len(), 1);
    assert_eq!(main_len(&c, &ks).unwrap(), 150_000);

    // the pipeline's cache hands out the same resolved container
    let mut conts = Containers::with_keys(&ks);
    let a = conts.get(&fx("rights_solid.nsz.bin")).unwrap();
    assert_eq!(a.tks.len(), 1);
    assert!(std::rc::Rc::ptr_eq(
        &a,
        &conts.get(&fx("rights_solid.nsz.bin")).unwrap()
    ));
}

#[test]
fn personalized_ticket_without_key_is_skipped_and_named() {
    let ks = keys(false);
    // plain load and keys without the device key: ticket skipped, the NCA reports what is missing
    for c in [
        Container::load(&fx("rights_solid.nsz.bin")).unwrap(),
        Container::load_with_keys(&fx("rights_solid.nsz.bin"), Some(&ks)).unwrap(),
    ] {
        assert!(c.tks.is_empty());
        assert!(matches!(
            main_len(&c, &ks),
            Err(Error::MissingKey(m)) if m.contains("ticket")
        ));
    }
    // a wrong device key is an integrity error, not silence
    let mut text = std::fs::read_to_string(fx("synthetic_keys.txt")).unwrap();
    let mut kp = unhex(nca0_fixture::D);
    kp.extend(unhex(nca0_fixture::N));
    kp.extend([0, 1, 0, 1]);
    kp.resize(0x230, 0);
    kp[0x80] ^= 1;
    let hex: String = kp.iter().map(|b| format!("{b:02x}")).collect();
    text.push_str(&format!("eticket_rsa_keypair = {hex}\n"));
    let bad = KeySet::parse(&text).unwrap();
    assert!(matches!(
        Container::load_with_keys(&fx("rights_solid.nsz.bin"), Some(&bad)),
        Err(napple_build::error::BuildError { .. })
    ));
}

/// Truncations and byte flips of real-writer output must end in `Err`, never a panic. Loading
/// and reading `main` through the NCZ view are both exercised (the stream is decoded lazily).
#[test]
fn truncated_and_corrupt_real_containers_do_not_panic() {
    let ks = keys(true);
    let dir = std::env::temp_dir().join(format!("napple-cr-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let probe = |bytes: &[u8]| {
        let p = dir.join("probe.nsz");
        std::fs::write(&p, bytes).unwrap();
        if let Ok(c) = Container::load_with_keys(&p, Some(&ks)) {
            for s in c.ncas.values() {
                let _ = Nca::open(s.clone(), &ks, &c.tks);
            }
            let _ = main_len(&c, &ks);
        }
    };
    for f in [
        "c00_solid.nsz.bin",
        "c00_block.nsz.bin",
        "g4000_block.nsz.bin",
        "c00.xcz.bin",
        "rights_solid.nsz.bin",
    ] {
        let good = std::fs::read(fx(f)).unwrap();
        let n = good.len();
        for cut in [0, 3, 4, 0x20, 0x100, 0x1000, n / 4, n / 2, n - n / 8, n - 1] {
            probe(&good[..cut.min(n)]);
        }
        // flip one byte in the container header, the NCZ header region and the zstd data
        for at in [0x8, 0x10, 0x18, 0x200, 0x4000, 0x4010, 0x4048, n / 2, n - 9] {
            let mut bad = good.clone();
            if at < n {
                bad[at] ^= 0xFF;
                probe(&bad);
            }
        }
    }
    let _ = std::fs::remove_dir_all(&dir);
}
