#![no_main]
use libfuzzer_sys::fuzz_target;
#[allow(unused_imports)]
use napple_fuzz::*;

fuzz_target!(|data: &[u8]| {
    let _ = napple_core::domain::keys::KeySet::parse(&String::from_utf8_lossy(data));
});
