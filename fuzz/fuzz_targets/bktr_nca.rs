#![no_main]
use libfuzzer_sys::fuzz_target;
#[allow(unused_imports)]
use napple_fuzz::*;

fuzz_target!(|data: &[u8]| {
    use napple_build::bktr::{RawNca, open_patched};
    let ks = keys();
    let enc = encrypt_nca_header(data);
    let Ok(raw) = RawNca::open(src(&enc), &ks, &Default::default()) else {
        return;
    };
    let base = src(&[0x5au8; 8192]);
    for sec in &raw.sections {
        if let Ok(p) = open_patched(&raw, sec, base.clone()) {
            touch(&p.data);
            touch(&p.patch);
            let _ = p.delta_entries(0);
        }
    }
});
