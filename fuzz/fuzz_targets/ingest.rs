#![no_main]
use libfuzzer_sys::fuzz_target;
#[allow(unused_imports)]
use napple_fuzz::*;

fuzz_target!(|data: &[u8]| {
    // Whole pipeline entry point: container -> NCAs -> ExeFS/RomFS (header trick not applied: use the
    // nca/ncz targets for the encrypted layers).
    let p = std::env::temp_dir().join(format!("napple-fuzz-ingest-{}.bin", std::process::id()));
    if std::fs::write(&p, data).is_ok() {
        let _ = napple_core::application::ingest::ingest(&p, &keys());
    }
});
