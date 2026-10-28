#![no_main]
use libfuzzer_sys::fuzz_target;
#[allow(unused_imports)]
use napple_fuzz::*;

fuzz_target!(|data: &[u8]| {
    let _ = napple_core::infrastructure::npdm::parse_npdm(data);
    let _ = napple_core::infrastructure::npdm::parse_sac(data);
    let _ = napple_core::infrastructure::npdm::parse_kac(data);
});
