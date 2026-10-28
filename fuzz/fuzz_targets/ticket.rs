#![no_main]
use libfuzzer_sys::fuzz_target;
#[allow(unused_imports)]
use napple_fuzz::*;

fuzz_target!(|data: &[u8]| {
    if let Ok(t) = napple_core::infrastructure::ticket::parse_ticket(data) {
        let _ = napple_core::infrastructure::ticket::encrypted_title_key(&t, &keys());
    }
});
