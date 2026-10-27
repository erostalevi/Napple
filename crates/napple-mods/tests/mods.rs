use napple_mods::apply::{MissReason, ModuleBytes, SegmentKind};
use napple_mods::model::{LoadedMod, Selection};
use napple_mods::romfs::{self, Entry, Lookup};
use napple_mods::util::{hex_decode, sha256_hex};
use napple_mods::{apply_mods_to_module, dry_run_mods_for_module, pack, validate};
use std::io::Write;
use std::path::{Path, PathBuf};

const MAIN_ID: &str = "0123456789ABCDEF0123456789ABCDEF";
const SUB_ID: &str = "FEDCBA9876543210FEDCBA9876543210";

fn root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../docs/mods/examples")
}

fn id32(s: &str) -> [u8; 32] {
    napple_mods::util::norm_build_id(s).unwrap()
}

/// The synthetic module: text = nops, rodata and data = patterns, 0x1C00 bytes, text [0,0x1000), ro [0x1000,0x1800), data [0x1800,0x1C00).
fn synthetic_image() -> Vec<u8> {
    let mut img = vec![0u8; 0x1C00];
    for c in img[..0x1000].chunks_mut(4) {
        c.copy_from_slice(&[0x1F, 0x20, 0x03, 0xD5]);
    }
    for (i, b) in img[0x1000..0x1800].iter_mut().enumerate() {
        *b = (i * 7 + 3) as u8;
    }
    img[0x1010..0x1020].copy_from_slice(b"Hello Synthetic\0");
    for (i, b) in img[0x1800..].iter_mut().enumerate() {
        *b = (i * 5 + 1) as u8;
    }
    img
}

fn module() -> ModuleBytes {
    ModuleBytes::from_flat(&synthetic_image(), 0x1000, 0x1800)
}

fn tmp() -> tempfile::TempDir {
    tempfile::tempdir().unwrap()
}

fn write(dir: &Path, rel: &str, body: &[u8]) {
    let p = dir.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, body).unwrap();
}

fn base_mod(dir: &Path) {
    write(dir, "mod.json", br#"{"id":"t.mod","name":"T","version":"1.0.0","titleId":"0100E4A100000000","author":"a","license":"MIT","buildIds":["AA"]}"#);
}

fn zip_of(entries: &[(&str, &[u8], Option<u32>)]) -> Vec<u8> {
    let mut buf = Vec::new();
    {
        let mut z = zip::ZipWriter::new(std::io::Cursor::new(&mut buf));
        for (n, b, mode) in entries {
            let mut o = zip::write::SimpleFileOptions::default()
                .compression_method(zip::CompressionMethod::Deflated);
            if let Some(m) = mode {
                o = o.unix_permissions(*m);
            }
            z.start_file(*n, o).unwrap();
            z.write_all(b).unwrap();
        }
        z.finish().unwrap();
    }
    buf
}

#[test]
fn synthetic_image_matches_committed_file() {
    let p = root().join("synthetic-module.bin");
    if std::env::var_os("NAPMOD_BLESS").is_some() {
        std::fs::write(&p, synthetic_image()).unwrap();
    }
    assert_eq!(std::fs::read(&p).unwrap(), synthetic_image());
}

#[test]
fn example_mod_validates_clean_and_packs_deterministically() {
    let m = LoadedMod::load(&root().join("synthetic-fix")).unwrap();
    let rep = validate::validate(&m);
    assert!(rep.ok(), "{:?}", rep.errors);
    assert!(rep.warnings.is_empty(), "{:?}", rep.warnings);
    assert!(rep.capabilities.code_patches && rep.capabilities.cheats && rep.capabilities.romfs);
    assert!(rep.capabilities.config && rep.capabilities.shaders);
    let d = tmp();
    let (a, b) = (d.path().join("a.napmod"), d.path().join("b.napmod"));
    let ha = pack::pack_dir(&root().join("synthetic-fix"), &a).unwrap();
    let hb = pack::pack_dir(&root().join("synthetic-fix"), &b).unwrap();
    assert_eq!(ha, hb);
    let packed = LoadedMod::load(&a).unwrap();
    let r2 = validate::validate(&packed);
    assert!(r2.ok(), "{:?}", r2.errors);
    assert_eq!(r2.hash_manifest, "ok");
    assert_eq!(packed.package_sha256.as_deref(), Some(ha.as_str()));
    // components were derived and written into the packaged mod.json
    assert_eq!(
        packed.manifest.components.as_ref().unwrap().patches.len(),
        2
    );
}

#[test]
fn golden_patch_application_on_synthetic_module() {
    let m = LoadedMod::load(&root().join("synthetic-fix")).unwrap();
    let sel = Selection {
        enabled: true,
        code_patches: true,
        ..Default::default()
    };
    let r = m.resolve(sel).unwrap();
    let mut mb = module();
    let before = mb.sha256();
    let dry = dry_run_mods_for_module(&id32(MAIN_ID), &mb, std::slice::from_ref(&r));
    assert_eq!(mb.sha256(), before, "dry run must not change the module");
    let rep = apply_mods_to_module(&id32(MAIN_ID), &mut mb, std::slice::from_ref(&r));
    assert_eq!(dry.sha256_after, rep.sha256_after);
    assert_eq!(rep.hits.len(), 3);
    assert_eq!(rep.misses.len(), 1);
    assert_eq!(rep.misses[0].reason, MissReason::OutOfRange);
    assert_eq!(
        rep.skipped.len(),
        2,
        "the @disabled block and the IPS file of another module"
    );
    assert!(rep.skipped.iter().any(|x| x.reason.contains("@disabled")));
    assert_eq!(rep.bytes_patched, 15);
    assert_eq!(rep.hits[2].segment, SegmentKind::Rodata);
    let flat = mb.flatten();
    assert_eq!(&flat[0x40..0x44], &hex_decode("00008052").unwrap()[..]);
    assert_eq!(&flat[0x100..0x104], &hex_decode("C0035FD6").unwrap()[..]);
    assert_eq!(&flat[0x1010..0x1017], b"Patched");
    assert_eq!(&flat[0x1017..0x1020], b"ynthetic\0");
    assert_eq!(
        &flat[0x1800..0x1804],
        &synthetic_image()[0x1800..0x1804],
        "disabled block untouched"
    );
    // golden hash of the whole patched module
    assert_eq!(rep.sha256_after, sha256_hex(&flat));
    assert_eq!(
        rep.sha256_after,
        "49837786c2137309502568c4f92e9387c3d63b16265413438f6dd15aa45e3600"
    );
    // applying the same patches again is idempotent
    let again = apply_mods_to_module(&id32(MAIN_ID), &mut mb, std::slice::from_ref(&r));
    assert_eq!(again.sha256_after, rep.sha256_after);
}

#[test]
fn code_patches_need_explicit_enable() {
    let m = LoadedMod::load(&root().join("synthetic-fix")).unwrap();
    let mut mb = module();
    let before = mb.sha256();
    for sel in [
        Selection::default(),
        Selection {
            enabled: true,
            ..Default::default()
        },
    ] {
        let r = m.resolve(sel).unwrap();
        let rep = apply_mods_to_module(&id32(MAIN_ID), &mut mb, std::slice::from_ref(&r));
        assert!(rep.hits.is_empty());
        assert_eq!(mb.sha256(), before);
    }
    // per-block override turns the @disabled block on, and switches an @enabled one off
    let mut sel = Selection {
        enabled: true,
        code_patches: true,
        ..Default::default()
    };
    sel.overrides
        .insert("patches/fix.pchtxt#Debug flag on [docs]".into(), true);
    sel.overrides.insert(
        "patches/fix.pchtxt#Rename the title string [docs]".into(),
        false,
    );
    let r = m.resolve(sel).unwrap();
    let rep = apply_mods_to_module(&id32(MAIN_ID), &mut mb, std::slice::from_ref(&r));
    assert_eq!(rep.hits.len(), 3);
    assert_eq!(mb.flatten()[0x1800], 1);
    assert_eq!(&mb.flatten()[0x1010..0x1015], b"Hello");
}

#[test]
fn ips_applies_only_to_its_module() {
    let m = LoadedMod::load(&root().join("synthetic-fix")).unwrap();
    let r = m
        .resolve(Selection {
            enabled: true,
            code_patches: true,
            ..Default::default()
        })
        .unwrap();
    let mut mb = module();
    let rep = apply_mods_to_module(&id32(SUB_ID), &mut mb, std::slice::from_ref(&r));
    assert_eq!(rep.hits.len(), 2, "the two nops of the IPS file");
    assert_eq!(rep.hits[0].module_offset, 0x200);
    let rep2 = apply_mods_to_module(&id32("00"), &mut module(), std::slice::from_ref(&r));
    assert!(rep2.hits.is_empty());
}

#[test]
fn overlap_and_header_and_crossing_are_reported() {
    let d = tmp();
    base_mod(d.path());
    write(
        d.path(),
        "patches/a.pchtxt",
        b"@nsobid-AA\n@enabled\n00000200 11111111\n00000202 2222\n00000050 00\n@enabled\n000000FE 00\n00000800 00\n00002900 ABCD\n00001AFF 0102\n",
    );
    let m = LoadedMod::load(d.path()).unwrap();
    let r = m
        .resolve(Selection {
            enabled: true,
            code_patches: true,
            ..Default::default()
        })
        .unwrap();
    let mut mb = module();
    let rep = apply_mods_to_module(&id32("AA"), &mut mb, &[r]);
    assert_eq!(rep.overlaps.len(), 1);
    assert!(!rep.overlaps[0].identical);
    assert_eq!(rep.overlaps[0].module_offset, 0x102);
    assert!(rep.misses.iter().any(|x| x.reason == MissReason::InHeader));
    assert!(
        rep.misses
            .iter()
            .any(|x| x.reason == MissReason::OutOfRange)
    );
    // 0x1AFF is a file offset -> module 0x19FF inside data; 0x2900 -> 0x2800 is beyond the image
    assert!(rep.hits.iter().any(|h| h.file_offset == 0x1AFF));
    // the later write wins
    assert_eq!(&mb.flatten()[0x100..0x104], &[0x11, 0x11, 0x22, 0x22]);
}

#[test]
fn segment_crossing_is_a_miss() {
    let d = tmp();
    base_mod(d.path());
    // module offset 0xFFE..0x1002 crosses text/rodata
    write(
        d.path(),
        "patches/a.pchtxt",
        b"@nsobid-AA\n@enabled\n00000FFE+0 00000000\n",
    );
    assert!(LoadedMod::load(d.path()).unwrap().load_patches().is_err());
    write(
        d.path(),
        "patches/a.pchtxt",
        b"@nsobid-AA\n@enabled\n000010FE 00000000\n",
    );
    let m = LoadedMod::load(d.path()).unwrap();
    let r = m
        .resolve(Selection {
            enabled: true,
            code_patches: true,
            ..Default::default()
        })
        .unwrap();
    let rep = apply_mods_to_module(&id32("AA"), &mut module(), &[r]);
    assert_eq!(rep.misses.len(), 1);
    assert_eq!(rep.misses[0].reason, MissReason::CrossesSegment);
}

#[test]
fn path_traversal_mods_are_rejected() {
    let bad = [
        "../evil.txt",
        "files/../../evil.txt",
        "/etc/passwd",
        "files\\..\\evil",
        "files/a/./b",
        "C:/x",
    ];
    for name in bad {
        let z = zip_of(&[
            (
                "mod.json",
                br#"{"id":"abc","name":"n","version":"1.0.0","titleId":"0100E4A100000000"}"#,
                None,
            ),
            (name, b"x", None),
        ]);
        let d = tmp();
        let p = d.path().join("m.napmod");
        std::fs::write(&p, z).unwrap();
        assert!(
            LoadedMod::load(&p).is_err(),
            "{name} must be refused: {:?}",
            LoadedMod::open(&p).map(|m| m.listing.entries)
        );
        let m = LoadedMod::open(&p).unwrap();
        let rep = validate::validate(&m);
        assert!(!rep.ok(), "{name}");
        assert!(
            rep.errors.iter().any(|e| e.contains("unsafe entry")),
            "{name}: {:?}",
            rep.errors
        );
    }
    // a symlink entry
    let mut buf = Vec::new();
    {
        let mut z = zip::ZipWriter::new(std::io::Cursor::new(&mut buf));
        let o = zip::write::SimpleFileOptions::default();
        z.start_file("mod.json", o).unwrap();
        z.write_all(br#"{"id":"abc","name":"n","version":"1.0.0","titleId":"0100E4A100000000"}"#)
            .unwrap();
        z.add_symlink("files/link", "/etc/passwd", o).unwrap();
        z.finish().unwrap();
    }
    let z = buf;
    let d = tmp();
    let p = d.path().join("m.napmod");
    std::fs::write(&p, z).unwrap();
    assert!(LoadedMod::load(&p).is_err());
    // the overlay never sees an unsafe path
    assert!(romfs::overlay_path("files/../x").is_err());
    assert!(romfs::overlay_path("files//x").is_err());
    assert_eq!(
        romfs::overlay_path("files/a/b.bin").unwrap(),
        Some(("/a/b.bin".into(), false))
    );
    assert_eq!(
        romfs::overlay_path("files/a/b.bin.delete").unwrap(),
        Some(("/a/b.bin".into(), true))
    );
    assert_eq!(romfs::overlay_path("patches/x").unwrap(), None);
}

#[test]
fn packaged_mod_must_carry_a_matching_hash_manifest() {
    let mj: &[u8] = br#"{"id":"abc","name":"n","version":"1.0.0","titleId":"0100E4A100000000","author":"a","license":"MIT","buildIds":["AA"]}"#;
    let d = tmp();
    let check = |entries: &[(&str, &[u8], Option<u32>)]| {
        let p = d.path().join("m.napmod");
        std::fs::write(&p, zip_of(entries)).unwrap();
        validate::validate(&LoadedMod::open(&p).unwrap())
    };
    assert!(
        check(&[("mod.json", mj, None)])
            .errors
            .iter()
            .any(|e| e.contains("manifest.sha256 is missing"))
    );
    let good = format!("{}  mod.json\n", sha256_hex(mj));
    let rep = check(&[
        ("mod.json", mj, None),
        ("manifest.sha256", good.as_bytes(), None),
    ]);
    assert_eq!(rep.hash_manifest, "ok");
    let bad = format!("{}  mod.json\n", "0".repeat(64));
    let rep = check(&[
        ("mod.json", mj, None),
        ("manifest.sha256", bad.as_bytes(), None),
    ]);
    assert_eq!(rep.hash_manifest, "mismatch");
    assert!(rep.errors.iter().any(|e| e.contains("hash mismatch")));
    let rep = check(&[
        ("mod.json", mj, None),
        ("config.json", b"{}", None),
        ("manifest.sha256", good.as_bytes(), None),
    ]);
    assert!(
        rep.errors
            .iter()
            .any(|e| e.contains("not listed in manifest.sha256"))
    );
}

#[test]
fn policy_errors_and_warnings() {
    let d = tmp();
    base_mod(d.path());
    write(d.path(), "files/prod.keys", b"x");
    write(d.path(), "files/game.nca", b"x");
    write(d.path(), "files/romfs.bin", b"x");
    write(d.path(), "patches/readme.txt", b"x");
    let rep = validate::validate(&LoadedMod::open(d.path()).unwrap());
    assert!(rep.errors.iter().any(|e| e.contains("key files")));
    assert!(rep.warnings.iter().any(|w| w.contains("game.nca")));
    assert!(rep.warnings.iter().any(|w| w.contains("romfs.bin")));
    assert!(rep.warnings.iter().any(|w| w.contains("readme.txt")));
    // declared components must match the files
    let d = tmp();
    write(d.path(), "mod.json", br#"{"id":"t.mod","name":"T","version":"1.0.0","titleId":"0100E4A100000000","components":{"patches":["patches/x.pchtxt"],"romfs":true}}"#);
    write(d.path(), "config.json", b"{}");
    write(
        d.path(),
        "cheats/0123456789ABCDEF.txt",
        b"[a]\n04000000 00000000 00000001\n",
    );
    let rep = validate::validate(&LoadedMod::open(d.path()).unwrap());
    assert!(rep.errors.iter().any(|e| e.contains("x.pchtxt")));
    assert!(rep.errors.iter().any(|e| e.contains("components.romfs")));
    assert!(rep.errors.iter().any(|e| e.contains("components.config")));
    assert!(
        rep.errors
            .iter()
            .any(|e| e.contains("cheats/0123456789ABCDEF.txt is a cheat file not declared"))
    );
    // bad component contents are errors, not panics
    let d = tmp();
    base_mod(d.path());
    write(
        d.path(),
        "patches/x.pchtxt",
        b"@nsobid-AA\n@enabled\nzz 00\n",
    );
    write(d.path(), "patches/nothex.ips", b"PATCHEOF");
    write(d.path(), "cheats/short.txt", b"[a]\n");
    write(d.path(), "config.json", b"{\"renderScale\": 99}");
    write(d.path(), "shaders/abcd.msl", b"nope");
    let rep = validate::validate(&LoadedMod::open(d.path()).unwrap());
    assert!(!rep.ok());
    // signature field is reserved
    let d = tmp();
    write(d.path(), "mod.json", br#"{"id":"t.mod","name":"T","version":"1.0.0","titleId":"0100E4A100000000","signature":{"alg":"x"}}"#);
    assert!(!validate::validate(&LoadedMod::open(d.path()).unwrap()).ok());
}

#[test]
fn romfs_overlay_order_deletes_and_lookup() {
    let (a, b) = (tmp(), tmp());
    for d in [&a, &b] {
        base_mod(d.path());
    }
    write(a.path(), "files/ui/a.txt", b"A");
    write(a.path(), "files/ui/both.txt", b"A");
    write(a.path(), "files/audio/theme.bin", b"A");
    write(a.path(), "files/audio/other.bin", b"A");
    write(b.path(), "files/ui/both.txt", b"B");
    write(b.path(), "files/audio.delete", b"");
    write(b.path(), "files/ui/a.txt.delete", b"");
    write(b.path(), "files/new/x.bin", b"BB");
    let (ma, mb) = (
        LoadedMod::load(a.path()).unwrap(),
        LoadedMod::load(b.path()).unwrap(),
    );
    let map = romfs::build_overlay(&[&ma, &mb]).unwrap();
    assert!(matches!(
        map.lookup("/ui/both.txt"),
        Lookup::Replaced(Entry::File { mod_index: 1, .. })
    ));
    assert!(matches!(map.lookup("/ui/a.txt"), Lookup::Deleted));
    assert!(matches!(map.lookup("/audio/theme.bin"), Lookup::Deleted));
    assert!(matches!(map.lookup("/audio"), Lookup::Deleted));
    assert!(matches!(map.lookup("/untouched/y"), Lookup::Original));
    assert_eq!(map.conflicts.len(), 1);
    assert_eq!((map.conflicts[0].winner, map.conflicts[0].loser), (1, 0));
    let Lookup::Replaced(e) = map.lookup("/new/x.bin") else {
        panic!()
    };
    assert_eq!(romfs::read_entry(&[&ma, &mb], e, 100).unwrap(), b"BB");
    // reversed order: A wins both.txt and brings audio files back (nothing deleted before it)
    let rev = romfs::build_overlay(&[&mb, &ma]).unwrap();
    assert!(matches!(
        rev.lookup("/audio/theme.bin"),
        Lookup::Replaced(_)
    ));
    assert!(matches!(rev.lookup("/ui/a.txt"), Lookup::Replaced(_)));
}

#[test]
fn cheats_are_gated_by_selection() {
    let m = LoadedMod::load(&root().join("synthetic-fix")).unwrap();
    let id = id32(MAIN_ID);
    assert!(
        m.resolve(Selection {
            enabled: true,
            ..Default::default()
        })
        .unwrap()
        .cheats_for(&id)
        .is_empty()
    );
    let r = m
        .resolve(Selection {
            enabled: true,
            cheats: true,
            ..Default::default()
        })
        .unwrap();
    assert_eq!(r.cheats_for(&id).len(), 2);
    assert!(r.cheats_for(&id32("99")).is_empty());
}

#[test]
fn corrupted_packages_never_panic() {
    let d = tmp();
    let p = d.path().join("ex.napmod");
    pack::pack_dir(&root().join("synthetic-fix"), &p).unwrap();
    let good = std::fs::read(&p).unwrap();
    let mut s = 0x1357_9bdf_2468_ace1u64;
    let mut next = || {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        s
    };
    for i in 0..300 {
        let mut m = good.clone();
        for _ in 0..(next() % 6 + 1) {
            let at = next() as usize % m.len();
            m[at] = next() as u8;
        }
        if i % 5 == 0 {
            m.truncate(next() as usize % m.len());
        }
        let q = d.path().join("c.napmod");
        std::fs::write(&q, &m).unwrap();
        if let Ok(lm) = LoadedMod::open(&q) {
            let _ = validate::validate(&lm);
            let _ = lm.resolve(Selection::default());
        }
    }
}

#[test]
fn cli_round_trip() {
    let d = tmp();
    let out = d.path().join("x.napmod");
    let a = |v: &[&str]| napple_mods::run(&v.iter().map(|s| s.to_string()).collect::<Vec<_>>());
    let ex = root().join("synthetic-fix");
    a(&["pack", ex.to_str().unwrap(), "-o", out.to_str().unwrap()]).unwrap();
    a(&["inspect", out.to_str().unwrap()]).unwrap();
    a(&["inspect", out.to_str().unwrap(), "--json"]).unwrap();
    a(&["validate", out.to_str().unwrap()]).unwrap();
    let img = root().join("synthetic-module.bin");
    let patched = d.path().join("patched.bin");
    a(&[
        "apply",
        out.to_str().unwrap(),
        "--image",
        img.to_str().unwrap(),
        "--build-id",
        MAIN_ID,
        "--layout",
        "0x1000,0x1800",
        "--enable-code",
        "-o",
        patched.to_str().unwrap(),
    ])
    .unwrap();
    assert_eq!(
        sha256_hex(&std::fs::read(&patched).unwrap()),
        "49837786c2137309502568c4f92e9387c3d63b16265413438f6dd15aa45e3600"
    );
    assert!(
        a(&[
            "apply",
            out.to_str().unwrap(),
            "--image",
            img.to_str().unwrap(),
            "--build-id",
            MAIN_ID
        ])
        .is_err(),
        "needs --dry-run or -o"
    );
    // convert round trip
    let (px, js, ip, px2) = (
        d.path().join("a.pchtxt"),
        d.path().join("a.json"),
        d.path().join("a.ips"),
        d.path().join("b.pchtxt"),
    );
    std::fs::write(&px, "@nsobid-AA\n// one\n@enabled\n00000200 11223344\n").unwrap();
    a(&["convert", px.to_str().unwrap(), js.to_str().unwrap()]).unwrap();
    a(&["convert", js.to_str().unwrap(), ip.to_str().unwrap()]).unwrap();
    a(&[
        "convert",
        ip.to_str().unwrap(),
        px2.to_str().unwrap(),
        "--build-id",
        "AA",
    ])
    .unwrap();
    let again = napple_mods::pchtxt::parse(&std::fs::read_to_string(&px2).unwrap()).unwrap();
    assert_eq!(
        again.builds[0].patches[0].records[0].data,
        hex_decode("11223344").unwrap()
    );
    assert!(a(&["convert", ip.to_str().unwrap(), px2.to_str().unwrap()]).is_err());
    // a bad mod fails validation with an error
    let bad = d.path().join("bad");
    write(&bad, "mod.json", b"{}");
    assert!(a(&["validate", bad.to_str().unwrap()]).is_err());
    assert!(a(&["nope"]).is_err());
    // build-id from a minimal NSO header
    let mut nso = vec![0u8; 0x100];
    nso[..4].copy_from_slice(b"NSO0");
    nso[0x40..0x60].copy_from_slice(&id32(MAIN_ID));
    let nf = d.path().join("main");
    std::fs::write(&nf, nso).unwrap();
    a(&["build-id", nf.to_str().unwrap()]).unwrap();
}
