#![no_main]
use libfuzzer_sys::fuzz_target;
#[allow(unused_imports)]
use napple_fuzz::*;

fuzz_target!(|data: &[u8]| {
    if let Ok(parts) = napple_core::infrastructure::xci::parse_xci(src(data)) {
        for (_, fs) in &parts {
            for e in fs.entries.clone() {
                if let Ok(s) = fs.open(&e.name) {
                    touch(&s);
                }
            }
        }
    }
});
