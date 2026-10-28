#![no_main]
use libfuzzer_sys::fuzz_target;
#[allow(unused_imports)]
use napple_fuzz::*;

fuzz_target!(|data: &[u8]| {
    if let Ok(z) = napple_build::zipread::ZipArchive::from_source(src(data)) {
        for e in &z.entries {
            if let Ok(s) = z.open_entry(e) {
                touch(&s);
            }
        }
    }
});
