#![no_main]
use libfuzzer_sys::fuzz_target;
#[allow(unused_imports)]
use napple_fuzz::*;

fuzz_target!(|data: &[u8]| {
    if let Ok(Some(iv)) = napple_core::infrastructure::ivfc::Ivfc::parse(data) {
        let _ = iv.data();
        let s = src(data);
        let _ = napple_core::infrastructure::ivfc::verify_tree(&s, &iv);
        let _ = napple_core::infrastructure::ivfc::verify_data(&s, &iv);
    }
});
