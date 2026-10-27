use napple_mods::apply::ModuleBytes;
use napple_mods::buildglue::{ModSet, OVERLAY_JSON};
use std::path::Path;

const ID: &str = "0123456789ABCDEF0123456789ABCDEF";

fn write(dir: &Path, rel: &str, body: &[u8]) {
    let p = dir.join(rel);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(p, body).unwrap();
}

fn mk(dir: &Path, id: &str, title: &str, extra: &str) {
    write(
        dir,
        "mod.json",
        format!(r#"{{"format":1,"id":"{id}","name":"N","version":"1.0.0","titleId":"{title}","buildIds":["{ID}"],"author":"a","license":"MIT"}}"#).as_bytes(),
    );
    write(
        dir,
        "patches/p.pchtxt",
        format!("@nsobid-{ID}\n@flag offset_shift 0x100\n\n@enabled\n00000010 {extra}\n@stop\n")
            .as_bytes(),
    );
    write(dir, "files/a/b.txt", extra.as_bytes());
}

#[test]
fn applies_patch_keys_by_set_and_writes_overlay() {
    let t = tempfile::tempdir().unwrap();
    let (a, b) = (t.path().join("a"), t.path().join("b"));
    mk(&a, "t.a", "0100E4A100000000", "DEADBEEF");
    mk(&b, "t.b", "0100E4A100000000", "01020304");
    let id: [u8; 32] = napple_mods::util::norm_build_id(ID).unwrap();

    let one = ModSet::load(std::slice::from_ref(&a), Some(0x0100E4A100000000)).unwrap();
    let two = ModSet::load(&[a.clone(), b.clone()], None).unwrap();
    assert_ne!(one.key(), two.key());
    assert_eq!(
        one.key(),
        ModSet::load(std::slice::from_ref(&a), None).unwrap().key()
    );
    assert_eq!(ModSet::empty().key(), "");

    let mut m = ModuleBytes::from_flat(&[0u8; 0x100], 0x80, 0xC0);
    let rep = two.apply(&id, &mut m);
    assert_eq!(rep.hits.len(), 2);
    assert_eq!(rep.overlaps.len(), 1, "later mod wins");
    assert_eq!(&m.flatten()[0x10..0x14], &[1, 2, 3, 4]);
    assert!(
        two.manifest_json(&[("main".into(), rep)])
            .contains("\"t.b\"")
    );

    let out = t.path().join("out");
    assert_eq!(two.write_overlay(&out).unwrap(), 1);
    let j = std::fs::read_to_string(out.join(OVERLAY_JSON)).unwrap();
    assert!(
        j.contains("\"path\":\"/a/b.txt\"") && j.contains("mods/files/1/a/b.txt"),
        "{j}"
    );
    assert_eq!(
        std::fs::read(out.join("mods/files/1/a/b.txt")).unwrap(),
        b"01020304"
    );
}

#[test]
fn wrong_title_and_duplicate_ids_are_refused() {
    let t = tempfile::tempdir().unwrap();
    let a = t.path().join("a");
    mk(&a, "t.a", "0100E4A100000000", "00");
    let e = ModSet::load(std::slice::from_ref(&a), Some(0x0100_0000_0000_2000))
        .err()
        .unwrap();
    assert!(e.0.contains("is for title"), "{e}");
    let e = ModSet::load(&[a.clone(), a], None).err().unwrap();
    assert!(e.0.contains("twice"), "{e}");
    assert!(ModSet::load(&[t.path().join("missing")], None).is_err());
}
