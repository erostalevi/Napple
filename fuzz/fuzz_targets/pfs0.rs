#![no_main]
use libfuzzer_sys::fuzz_target;
#[allow(unused_imports)]
use napple_fuzz::*;

fuzz_target!(|data: &[u8]| {
    if let Ok(fs) = napple_core::infrastructure::pfs::parse_pfs0(src(data)) {
        for e in fs.entries.clone() {
            if let Ok(s) = fs.open(&e.name) {
                touch(&s);
            }
        }
    }
});
