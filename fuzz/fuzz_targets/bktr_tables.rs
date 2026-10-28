#![no_main]
use libfuzzer_sys::fuzz_target;
#[allow(unused_imports)]
use napple_fuzz::*;

fuzz_target!(|data: &[u8]| {
    for count in [1u64, 2, 3, 7, 1 << 12, u64::MAX] {
        let _ = napple_build::bktr::parse_relocations(data, count);
        let _ = napple_build::bktr::parse_subsections(data, count);
    }
});
