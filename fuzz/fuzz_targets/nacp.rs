#![no_main]
use libfuzzer_sys::fuzz_target;
#[allow(unused_imports)]
use napple_fuzz::*;

fuzz_target!(|data: &[u8]| {
    if let Ok(n) = napple_core::infrastructure::nacp::parse_nacp(data) {
        let _ = (n.name(), n.publisher());
    }
});
