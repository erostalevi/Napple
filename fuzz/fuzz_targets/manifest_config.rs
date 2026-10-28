#![no_main]
use libfuzzer_sys::fuzz_target;
#[allow(unused_imports)]
use napple_fuzz::*;

fuzz_target!(|data: &[u8]| {
    if let Ok((m, _)) = napple_mods::manifest::parse(data) {
        let _ = napple_mods::manifest::check(&m);
        let _ = napple_mods::manifest::to_json(&m);
    }
    if let Ok((c, _)) = napple_mods::config::parse(&String::from_utf8_lossy(data)) {
        let _ = napple_mods::config::merge(&[&c, &c]);
    }
    let s = String::from_utf8_lossy(data);
    let _ = napple_mods::util::safe_rel_path(&s);
    let _ = napple_mods::util::norm_build_id(&s);
    let _ = napple_mods::romfs::overlay_path(&s);
});
