//! End-to-end `napple build` runs on synthetic fixtures (see tests/support): base + BKTR update + DLC.
mod support;
use support::*;

fn s(v: &[&str]) -> Vec<String> {
    v.iter().map(|x| x.to_string()).collect()
}

fn run(args: &[&str]) -> Result<(), String> {
    napple_build::run(&s(args))
}

fn p(x: &std::path::Path) -> &str {
    x.to_str().unwrap()
}

fn build_dirs(out: &std::path::Path) -> Vec<String> {
    let mut v: Vec<String> = std::fs::read_dir(out.join(format!("{APP_ID:016X}")))
        .unwrap()
        .flatten()
        .map(|e| e.file_name().to_string_lossy().to_string())
        .collect();
    v.sort();
    v
}

#[test]
fn base_update_dlc_data_only_materialized() {
    let _t = tmp("a");
    let g = make_game(&_t, true);
    let out = tmp("a-out");
    run(&[
        p(&g.base_nsp),
        p(&g.update_nsp),
        p(&g.dlc_nsp),
        "-k",
        p(&g.keys),
        "--updates",
        "all",
        "--dlc",
        "all",
        "--data-only",
        "-o",
        p(&out),
    ])
    .unwrap();
    let root = out.join(format!("{APP_ID:016X}"));
    let dirs = build_dirs(&out);
    assert!(
        dirs.contains(&"default".to_string())
            && dirs.contains(&"napple-app.json".to_string())
            && dirs.contains(&"dlc".to_string())
    );
    // base: build id from the main NSO (tag 1 -> 01 02 03 ...)
    let base = root.join("0102030405060708");
    let upd = root.join("32333435363738 39".replace(' ', "").as_str());
    assert_eq!(std::fs::read(base.join("romfs.bin")).unwrap(), g.base_romfs);
    assert_eq!(
        std::fs::read(upd.join("romfs.bin")).unwrap(),
        g.update_romfs,
        "BKTR-patched RomFS"
    );
    assert_eq!(std::fs::read(base.join("main.npdm")).unwrap(), b"NPDMDATA");
    for d in [&base, &upd] {
        let m = std::fs::read_to_string(d.join("manifest.json")).unwrap();
        assert!(
            m.contains(&format!("\"title_id\": \"{APP_ID:016X}\""))
                || m.contains(&format!("\"title_id\":\"{APP_ID:016X}\""))
        );
        assert!(m.contains("\"load_order\""), "{m}");
        for n in ["main", "subsdk0"] {
            let t = std::fs::read(d.join(format!("modules/{n}.tables"))).unwrap();
            assert_eq!(&t[..4], b"NTB1");
            assert!(d.join(format!("modules/{n}.patched.bin")).is_file());
        }
    }
    // the update has its own build id and its own tables
    assert_ne!(
        std::fs::read(base.join("modules/main.tables")).unwrap(),
        std::fs::read(upd.join("modules/main.tables")).unwrap()
    );
    assert_eq!(
        std::fs::read(root.join("dlc/0100000000003001/v0/romfs.bin")).unwrap(),
        g.dlc_romfs
    );
    let app = std::fs::read_to_string(root.join("napple-app.json")).unwrap();
    assert!(
        app.contains("\"kind\":\"update\"")
            && app.contains("\"version\":65536")
            && app.contains("0100000000003001"),
        "{app}"
    );
    assert_eq!(
        std::fs::read_to_string(root.join("default"))
            .unwrap()
            .trim(),
        "3233343536373839"
    );
    // no staging leftovers next to the output
    assert!(
        std::fs::read_dir(&out)
            .unwrap()
            .flatten()
            .all(|e| !e.file_name().to_string_lossy().starts_with(".napple-build"))
    );
}

#[test]
fn update_choice_none_skips_the_update() {
    let _t = tmp("b");
    let g = make_game(&_t, true);
    let out = tmp("b-out");
    run(&[
        p(&g.base_nsp),
        p(&g.update_nsp),
        "-k",
        p(&g.keys),
        "--updates",
        "none",
        "--dlc",
        "none",
        "--data-only",
        "-o",
        p(&out),
    ])
    .unwrap();
    let dirs = build_dirs(&out);
    assert_eq!(
        dirs.iter()
            .filter(|d| d.len() == 16 && d.bytes().all(|b| b.is_ascii_hexdigit()))
            .count(),
        1,
        "{dirs:?}"
    );
    assert!(!out.join(format!("{APP_ID:016X}/dlc")).exists());
}

#[test]
fn delta_layer_reconstructs_the_patched_romfs() {
    let _t = tmp("c");
    let g = make_game(&_t, true);
    let out = tmp("c-out");
    run(&[
        p(&g.base_nsp),
        p(&g.update_nsp),
        "-k",
        p(&g.keys),
        "--updates",
        "all",
        "--update-romfs",
        "delta",
        "--data-only",
        "-o",
        p(&out),
    ])
    .unwrap();
    let upd = out.join(format!("{APP_ID:016X}/3233343536373839"));
    assert!(!upd.join("romfs.bin").exists());
    let reloc = std::fs::read(upd.join("romfs.delta/romfs.reloc")).unwrap();
    let patch = std::fs::read(upd.join("romfs.delta/patch.bin")).unwrap();
    let base =
        std::fs::read(out.join(format!("{APP_ID:016X}/0102030405060708/romfs.bin"))).unwrap();
    assert_eq!(&reloc[..4], b"RLC1");
    let count = u32::from_le_bytes(reloc[4..8].try_into().unwrap()) as usize;
    let size = u64::from_le_bytes(reloc[8..16].try_into().unwrap()) as usize;
    assert_eq!(size, g.update_romfs.len());
    let ent: Vec<(usize, usize, u32)> = (0..count)
        .map(|i| {
            let o = 16 + i * 24;
            (
                u64::from_le_bytes(reloc[o..o + 8].try_into().unwrap()) as usize,
                u64::from_le_bytes(reloc[o + 8..o + 16].try_into().unwrap()) as usize,
                u32::from_le_bytes(reloc[o + 16..o + 20].try_into().unwrap()),
            )
        })
        .collect();
    let mut rebuilt: Vec<u8> = Vec::new();
    for (i, (off, src_off, source)) in ent.iter().enumerate() {
        let end = ent.get(i + 1).map(|e| e.0).unwrap_or(size);
        assert_eq!(*off, rebuilt.len());
        let from = if *source == 0 { &base } else { &patch };
        rebuilt.extend(&from[*src_off..*src_off + (end - off)]);
    }
    assert_eq!(rebuilt, g.update_romfs);
}

#[test]
fn update_with_full_romfs_section_and_app_bundle() {
    // an update carrying a plain (non-BKTR) RomFS section is streamed as is
    let _t = tmp("d");
    let g = make_game(&_t, false);
    let out = tmp("d-out");
    // fake runtime / launcher executables; no signing
    let (rt, la) = (g.dir.join("rt"), g.dir.join("la"));
    std::fs::write(&rt, b"#!/bin/sh\n").unwrap();
    std::fs::write(&la, b"#!/bin/sh\n").unwrap();
    run(&[
        p(&g.base_nsp),
        p(&g.update_nsp),
        p(&g.dlc_nsp),
        "-k",
        p(&g.keys),
        "--updates",
        "latest",
        "--runtime",
        p(&rt),
        "--launcher",
        p(&la),
        "--no-sign",
        "-o",
        p(&out),
        "--name",
        "My/Test: Game",
    ])
    .unwrap();
    let app = out.join("My Test Game.app");
    assert!(
        app.is_dir(),
        "{:?}",
        std::fs::read_dir(&out)
            .unwrap()
            .flatten()
            .map(|e| e.file_name())
            .collect::<Vec<_>>()
    );
    let plist = std::fs::read_to_string(app.join("Contents/Info.plist")).unwrap();
    assert!(
        plist.contains("com.napple.game.0100000000002000")
            && plist.contains("<string>26.0</string>")
            && plist.contains("My Test Game")
    );
    assert!(
        app.join("Contents/MacOS/napple-launcher").is_file()
            && app.join("Contents/MacOS/napple-runtime").is_file()
    );
    assert_eq!(
        std::fs::read(app.join("Contents/PkgInfo")).unwrap(),
        b"APPL????"
    );
    let game = app.join("Contents/Resources/game");
    assert_eq!(
        std::fs::read(game.join("3233343536373839/romfs.bin")).unwrap(),
        g.update_romfs
    );
    let json = std::fs::read_to_string(game.join("napple-app.json")).unwrap();
    assert!(
        json.contains("built without --firmware"),
        "compat note about fonts missing: {json}"
    );
    assert!(json.contains("\"name\":\"My Test Game\""));
    // rebuilding over the same output replaces the app atomically
    run(&[
        p(&g.base_nsp),
        "-k",
        p(&g.keys),
        "--updates",
        "none",
        "--runtime",
        p(&rt),
        "--launcher",
        p(&la),
        "--no-sign",
        "-o",
        p(&out),
        "--name",
        "My/Test: Game",
    ])
    .unwrap();
    assert!(
        !app.join("Contents/Resources/game/3233343536373839")
            .exists()
    );
    assert!(
        std::fs::read_dir(&out)
            .unwrap()
            .flatten()
            .all(|e| !e.file_name().to_string_lossy().starts_with(".napple-"))
    );
}

#[test]
fn ios_build_reaches_the_dylib_step_and_rejects_the_tiny_fixture_layout() {
    // The synthetic NSO is 0x2300 bytes with data in the first 16 KiB page, a layout no real game has (the runtime refuses it too:
    // "text overlaps data"). So the full iOS bundle shape is covered by ios::tests::dylibize_converts_modules_and_nros and the
    // macho tests; here we prove the iOS path runs up to the dylib step and fails with a precise, machine-readable code.
    let _t = tmp("ios1");
    let g = make_game(&_t, false);
    let out = tmp("ios1-out");
    let rt = g.dir.join("rt-ios");
    std::fs::write(&rt, b"fake ios runtime").unwrap();
    let e = run(&[
        p(&g.base_nsp),
        "-k",
        p(&g.keys),
        "--updates",
        "none",
        "--target",
        "ipad",
        "--bundle-id",
        "com.me.napple",
        "--runtime",
        p(&rt),
        "--no-sign",
        "-o",
        p(&out),
        "--name",
        "iOS Game",
    ])
    .unwrap_err();
    assert!(
        e.starts_with("[macho]") && e.contains("text overlaps data"),
        "{e}"
    );
    assert!(
        !out.join("iOS Game.app").exists(),
        "a failed build publishes nothing"
    );
}

#[test]
fn keep_cache_allows_resume() {
    let _t = tmp("e");
    let g = make_game(&_t, true);
    let out = tmp("e-out");
    run(&[
        p(&g.base_nsp),
        "-k",
        p(&g.keys),
        "--updates",
        "none",
        "--data-only",
        "--keep-cache",
        "-o",
        p(&out),
    ])
    .unwrap();
    // cache lives in the staging root (temp dir when it shares the volume with the output); other tests build the same
    // title in parallel, so look only at this build's own cache directory
    let name = napple_build::pipeline::cache_dir_name(
        APP_ID,
        &[std::path::absolute(&g.base_nsp).unwrap()],
        &std::path::absolute(&out.0).unwrap(),
    );
    let found = [out.0.clone(), std::env::temp_dir()]
        .iter()
        .flat_map(|r| {
            std::fs::read_dir(r.join(".napple-cache.nosync").join(&name))
                .into_iter()
                .flatten()
                .flatten()
        })
        .map(|e| e.path())
        .find(|p| {
            p.extension().is_some_and(|e| e == "ok") && p.to_string_lossy().contains("romfs-")
        });
    let ok = found.expect("cache marker kept");
    let m1 = std::fs::metadata(ok.with_extension(""))
        .unwrap()
        .modified()
        .unwrap();
    run(&[
        p(&g.base_nsp),
        "-k",
        p(&g.keys),
        "--updates",
        "none",
        "--data-only",
        "--keep-cache",
        "-o",
        p(&out),
    ])
    .unwrap();
    assert_eq!(
        std::fs::metadata(ok.with_extension(""))
            .unwrap()
            .modified()
            .unwrap(),
        m1,
        "cached RomFS was reused, not rewritten"
    );
    let _ = std::fs::remove_dir_all(ok.parent().unwrap());
}

#[test]
fn errors_are_machine_readable() {
    let _t = tmp("f");
    let g = make_game(&_t, true);
    let out = tmp("f-out");
    let e = run(&[
        p(&g.update_nsp),
        "-k",
        p(&g.keys),
        "--data-only",
        "-o",
        p(&out),
    ])
    .unwrap_err();
    assert!(e.starts_with("[plan]"), "{e}");
    let e = run(&[
        p(&g.base_nsp),
        "-k",
        "/definitely/missing.keys",
        "--data-only",
        "-o",
        p(&out),
    ])
    .unwrap_err();
    assert!(e.starts_with("[keys]"), "{e}");
    let wrong = g.dir.join("wrong.keys");
    std::fs::write(
        &wrong,
        keys_text().replace(&hex(&HEADER_KEY), &hex(&[8u8; 32])),
    )
    .unwrap();
    let e = run(&[
        p(&g.base_nsp),
        "-k",
        p(&wrong),
        "--data-only",
        "-o",
        p(&out),
    ])
    .unwrap_err();
    assert!(e.contains("integrity") || e.contains("wrong keys"), "{e}");
    assert!(
        !e.contains(&hex(&HEADER_KEY)) && !e.contains(&hex(&KAK)),
        "no key material in errors"
    );
    // iOS needs signing inputs (checked before any heavy work) and unknown targets are usage errors
    let e = run(&[
        p(&g.base_nsp),
        "-k",
        p(&g.keys),
        "--target",
        "ios",
        "-o",
        p(&out),
    ])
    .unwrap_err();
    assert!(e.starts_with("[sign]"), "{e}");
    assert!(
        run(&[
            p(&g.base_nsp),
            "-k",
            p(&g.keys),
            "--target",
            "tvos",
            "-o",
            p(&out)
        ])
        .is_err()
    );
    let e = run(&[
        "/no/such/file.nsp",
        "-k",
        p(&g.keys),
        "--data-only",
        "-o",
        p(&out),
    ])
    .unwrap_err();
    assert!(
        e.starts_with("[input]") || e.starts_with("[container]"),
        "{e}"
    );
    assert!(run(&[p(&g.base_nsp)]).is_err());
}

#[test]
fn check_subcommand_reports_without_key_values() {
    let _t = tmp("h");
    let g = make_game(&_t, true);
    run(&["--check", "-k", p(&g.keys)]).unwrap();
    let bad = g.dir.join("bad.keys");
    std::fs::write(
        &bad,
        format!("key_area_key_application_02 = {}\n", hex(&KAK)),
    )
    .unwrap();
    let e = run(&["--check", "-k", p(&bad)]).unwrap_err();
    assert!(e.contains("header_key") && !e.contains(&hex(&KAK)), "{e}");
}

#[test]
fn bktr_base_offsets_in_data_level_space_are_detected() {
    let _t = tmp("bktr1");
    let g = make_game_with(&_t, true, true);
    let out = tmp("bktr1-out");
    run(&[
        p(&g.base_nsp),
        p(&g.update_nsp),
        "-k",
        p(&g.keys),
        "--updates",
        "all",
        "--data-only",
        "-o",
        p(&out),
    ])
    .unwrap();
    let upd = out.join(format!("{APP_ID:016X}/3233343536373839"));
    assert_eq!(
        std::fs::read(upd.join("romfs.bin")).unwrap(),
        g.update_romfs
    );
}

#[test]
fn corrupt_bktr_tables_fail_with_a_clear_code() {
    let _t = tmp("j");
    let g = make_game_full(&_t, true, false, true);
    let out = tmp("j-out");
    let e = run(&[
        p(&g.base_nsp),
        p(&g.update_nsp),
        "-k",
        p(&g.keys),
        "--updates",
        "all",
        "--data-only",
        "-o",
        p(&out),
    ])
    .unwrap_err();
    assert!(e.starts_with("[bktr]"), "{e}");
    // nothing published on failure
    assert!(!out.join(format!("{APP_ID:016X}")).exists());
    // the same build without the update still works
    run(&[
        p(&g.base_nsp),
        p(&g.update_nsp),
        "-k",
        p(&g.keys),
        "--updates",
        "none",
        "--data-only",
        "-o",
        p(&out),
    ])
    .unwrap();
}

#[test]
fn html_document_written_next_to_romfs_map() {
    let _t = tmp("html");
    let g = make_game_html(&_t);
    let out = tmp("html-out");
    run(&[
        p(&g.base_nsp),
        "-k",
        p(&g.keys),
        "--data-only",
        "-o",
        p(&out),
    ])
    .unwrap();
    let base = out.join(format!("{APP_ID:016X}")).join("0102030405060708");
    assert_eq!(
        std::fs::read(base.join("html/index.html")).unwrap(),
        b"<html>manual</html>"
    );
    assert_eq!(
        std::fs::read(base.join("html/a.png")).unwrap(),
        [0x89, b'P', b'N', b'G']
    );
    // a title without an HtmlDocument gets no html/ directory
    let _t2 = tmp("nohtml");
    let g2 = make_game(&_t2, false);
    let out2 = tmp("nohtml-out");
    run(&[
        p(&g2.base_nsp),
        "-k",
        p(&g2.keys),
        "--data-only",
        "-o",
        p(&out2),
    ])
    .unwrap();
    assert!(
        !out2
            .join(format!("{APP_ID:016X}"))
            .join("0102030405060708/html")
            .exists()
    );
}
