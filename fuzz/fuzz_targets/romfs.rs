#![no_main]
use libfuzzer_sys::fuzz_target;
#[allow(unused_imports)]
use napple_fuzz::*;

fuzz_target!(|data: &[u8]| {
    if let Ok(rf) = napple_core::infrastructure::romfs::RomFs::parse(src(data)) {
        for f in rf.files.iter().take(64) {
            if let Ok(x) = rf.open(f) {
                touch(&x);
            }
        }
    }
});
