#![no_main]
use libfuzzer_sys::fuzz_target;
#[allow(unused_imports)]
use napple_fuzz::*;

fuzz_target!(|data: &[u8]| {
    if let Ok(p) = napple_mods::ips::parse(data) {
        let _ = napple_mods::ips::write(&p);
        if p.records.iter().all(|r| r.offset < (1 << 20)) {
            let mut t = vec![0u8; 4096];
            let _ = napple_mods::ips::apply(&p, &mut t);
        }
    }
});
