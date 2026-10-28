#![no_main]
use libfuzzer_sys::fuzz_target;
#[allow(unused_imports)]
use napple_fuzz::*;

fuzz_target!(|data: &[u8]| {
    let p = std::env::temp_dir().join(format!("napple-fuzz-mod-{}.napmod", std::process::id()));
    if std::fs::write(&p, data).is_err() {
        return;
    }
    if let Ok(m) = napple_mods::model::LoadedMod::open(&p) {
        let _ = m.verify_sha_manifest();
        let _ = m.load_config();
        let _ = napple_mods::validate::validate(&m);
        let _ = napple_mods::romfs::build_overlay(&[&m]);
        let _ = m.resolve(Default::default());
    }
});
