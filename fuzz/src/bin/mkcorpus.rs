//! Writes the seed corpus (synthetic fixtures only) to fuzz/corpus/<target>/: `cargo run --release --bin mkcorpus`.
use napple_fuzz::decrypt_nca_header;
use std::path::{Path, PathBuf};

fn main() {
    // `mkcorpus encrypt <artifact> <out>`: turns a decrypted-header crash input of the nca/bktr_nca targets
    // into the encrypted file the readers (and the regression tests under tests/regress) take.
    let a: Vec<String> = std::env::args().collect();
    if a.len() == 4 && a[1] == "encrypt" {
        let d = std::fs::read(&a[2]).unwrap();
        std::fs::write(&a[3], napple_fuzz::encrypt_nca_header(&d)).unwrap();
        return;
    }
    let root = Path::new(env!("CARGO_MANIFEST_DIR"));
    let fx = root.join("../crates/napple-core/tests/fixtures/nsz");
    let out = |t: &str, name: &str, d: &[u8]| {
        let dir: PathBuf = root.join("corpus").join(t);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join(name), d).unwrap();
    };
    let rd = |n: &str| std::fs::read(fx.join(n)).unwrap();
    for n in [
        "c00_block.nsz.bin",
        "c00_solid.nsz.bin",
        "g4000_block.nsz.bin",
    ] {
        let d = rd(n);
        out("pfs0", n, &d[..d.len().min(0x400)]);
        out("hfs0", n, &d[..d.len().min(0x400)]);
        out("ingest", n, &d);
    }
    let x = rd("c00.xcz.bin");
    out("xci", "c00.xcz", &x[..x.len().min(0x20000)]);
    out("ingest", "c00.xcz", &x);
    for n in ["c00.nca.bin", "nca2.nca.bin", "g4000.nca.bin"] {
        let d = rd(n);
        // decrypted header + (truncated) body: the nca/bktr targets re-encrypt the header
        let mut p = decrypt_nca_header(&d);
        p.truncate(p.len().min(0x10000));
        out("nca", n, &p);
        out("bktr_nca", n, &p[..p.len().min(0x4000)]);
    }
    // NCZ members of the NSZ fixtures
    for n in ["c00_block.nsz.bin", "c00_solid.nsz.bin"] {
        let d = rd(n);
        let fs = napple_core::infrastructure::pfs::parse_pfs0(napple_fuzz::src(&d)).unwrap();
        let e = fs
            .entries
            .iter()
            .find(|e| e.name.ends_with(".ncz"))
            .unwrap();
        let mut m = fs.read(&e.name).unwrap();
        let dec = decrypt_nca_header(&m);
        m[..0xC00].copy_from_slice(&dec[..0xC00]);
        m.truncate(m.len().min(0x20000));
        out("ncz", n, &m);
    }
    out("keyfile", "synthetic", napple_fuzz::KEYS.as_bytes());
    let ex = root.join("../docs/mods/examples/synthetic-fix");
    for (t, rel) in [
        ("ips", "patches/FEDCBA9876543210FEDCBA9876543210.ips"),
        ("pchtxt", "patches/fix.pchtxt"),
        ("cheats", "cheats/0123456789ABCDEF.txt"),
        ("manifest_config", "mod.json"),
        ("manifest_config", "config.json"),
    ] {
        out(
            t,
            rel.rsplit('/').next().unwrap(),
            &std::fs::read(ex.join(rel)).unwrap(),
        );
    }
    // a .napmod zip of the example mod, built with the mods crate
    let zip = std::env::temp_dir().join("napple-fuzz-seed.napmod");
    napple_mods::pack::pack_dir(&ex, &zip).unwrap();
    out(
        "napmod",
        "synthetic-fix.napmod",
        &std::fs::read(&zip).unwrap(),
    );
    out(
        "zip_firmware",
        "synthetic-fix.zip",
        &std::fs::read(&zip).unwrap(),
    );
    println!("corpus written to {}", root.join("corpus").display());
}
