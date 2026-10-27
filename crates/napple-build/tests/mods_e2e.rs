//! `napple build --mods` end to end on the synthetic fixtures (tests/support): a pchtxt patch lands before the rewrite,
//! the applied set is recorded in manifest.json, the romfs overlay is written, and control.nacp sits in the data dir.
mod support;
use support::*;

fn s(v: &[&str]) -> Vec<String> {
    v.iter().map(|x| x.to_string()).collect()
}

fn p(x: &std::path::Path) -> &str {
    x.to_str().unwrap()
}

fn write(dir: &std::path::Path, rel: &str, body: &[u8]) {
    let f = dir.join(rel);
    std::fs::create_dir_all(f.parent().unwrap()).unwrap();
    std::fs::write(f, body).unwrap();
}

/// Build id of the base `main` (build_tag 1: bytes 1..=32).
fn main_bid() -> String {
    (1u8..=32).map(|b| format!("{b:02X}")).collect()
}

fn make_mod(dir: &std::path::Path) {
    write(
        dir,
        "mod.json",
        format!(
            r#"{{"format":1,"id":"test.glue","name":"Glue","version":"1.0.0","titleId":"{APP_ID:016X}","buildIds":["{}"],"author":"t","license":"MIT"}}"#,
            main_bid()
        )
        .as_bytes(),
    );
    // file offset = 0x100 (NSO header) + module offset 8: a zero word in the synthetic text becomes a nop
    write(
        dir,
        "patches/fix.pchtxt",
        format!(
            "@nsobid-{}\n@flag offset_shift 0x100\n\n@enabled\n00000008 1F2003D5\n@stop\n",
            main_bid()
        )
        .as_bytes(),
    );
    write(dir, "files/ui/title.txt", b"patched title");
    write(dir, "files/audio/old.bin.delete", b"");
}

fn build(g: &Game, out: &std::path::Path, extra: &[&str]) -> std::path::PathBuf {
    let mut a = vec![
        p(&g.base_nsp),
        "-k",
        p(&g.keys),
        "--data-only",
        "-o",
        p(out),
    ];
    a.extend_from_slice(extra);
    napple_build::run(&s(&a)).unwrap();
    out.join(format!("{APP_ID:016X}")).join("0102030405060708")
}

#[test]
fn patch_applies_before_rewrite_and_is_recorded() {
    let t = tmp("mg");
    let g = make_game(&t, false);
    let md = tmp("mg-mod");
    make_mod(&md);

    let (oa, ob) = (tmp("mg-out-a"), tmp("mg-out-b"));
    let plain = build(&g, &oa, &[]);
    let modded = build(&g, &ob, &["--mods", p(&md)]);

    let a = std::fs::read(plain.join("modules/main.patched.bin")).unwrap();
    let b = std::fs::read(modded.join("modules/main.patched.bin")).unwrap();
    assert_eq!(a[8..12], [0, 0, 0, 0]);
    assert_eq!(
        b[8..12],
        [0x1F, 0x20, 0x03, 0xD5],
        "patch must be in the rewritten image"
    );
    // the other module has another build id: untouched
    assert_eq!(
        std::fs::read(plain.join("modules/subsdk0.patched.bin")).unwrap(),
        std::fs::read(modded.join("modules/subsdk0.patched.bin")).unwrap()
    );
    let rep = std::fs::read_to_string(modded.join("modules/main.mods.json")).unwrap();
    assert!(rep.contains("fix.pchtxt"), "{rep}");

    let m = std::fs::read_to_string(modded.join("manifest.json")).unwrap();
    assert!(
        m.contains("\"mods\"") && m.contains("test.glue") && m.contains("\"mod_set\""),
        "{m}"
    );
    assert!(m.contains("mods/overlay.json"), "{m}");
    assert!(
        !std::fs::read_to_string(plain.join("manifest.json"))
            .unwrap()
            .contains("\"mods\"")
    );

    // overlay: replaced file copied beside the data dir, tombstone listed
    let ov = std::fs::read_to_string(modded.join("mods/overlay.json")).unwrap();
    assert!(
        ov.contains("\"/ui/title.txt\"") && ov.contains("\"delete\":\"/audio/old.bin\""),
        "{ov}"
    );
    assert_eq!(
        std::fs::read(modded.join("mods/files/0/ui/title.txt")).unwrap(),
        b"patched title"
    );
    assert!(!plain.join("mods").exists());
}

#[test]
fn control_nacp_is_written_beside_the_data_dir() {
    let t = tmp("mg-nacp");
    let g = make_game(&t, false);
    let o = tmp("mg-nacp-out");
    let d = build(&g, &o, &[]);
    assert_eq!(
        std::fs::read(d.join("control.nacp")).unwrap(),
        nacp("Test Game", "1.0.0")
    );
}

#[test]
fn mod_for_another_title_fails_the_build() {
    let t = tmp("mg-bad");
    let g = make_game(&t, false);
    let md = tmp("mg-bad-mod");
    write(
        &md,
        "mod.json",
        br#"{"format":1,"id":"x.other","name":"O","version":"1.0.0","titleId":"0100E4A100000000","buildIds":["AA"],"author":"t","license":"MIT"}"#,
    );
    let out = tmp("mg-bad-out");
    let e = napple_build::run(&s(&[
        p(&g.base_nsp),
        "-k",
        p(&g.keys),
        "--data-only",
        "-o",
        p(&out),
        "--mods",
        p(&md),
    ]))
    .unwrap_err();
    assert!(e.contains("mods") && e.contains("0100E4A100000000"), "{e}");
}
