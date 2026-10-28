#![no_main]
use libfuzzer_sys::fuzz_target;
#[allow(unused_imports)]
use napple_fuzz::*;

fuzz_target!(|data: &[u8]| {
    let t = String::from_utf8_lossy(data);
    if let Ok(p) = napple_mods::pchtxt::parse(&t) {
        let _ = napple_mods::pchtxt::parse(&napple_mods::pchtxt::write(&p));
        let _ = napple_mods::pchtxt::from_json(&napple_mods::pchtxt::to_json(&p));
    }
    if let Ok(v) = serde_json::from_slice(data) {
        let _ = napple_mods::pchtxt::from_json(&v);
    }
});
