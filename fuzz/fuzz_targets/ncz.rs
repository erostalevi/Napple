#![no_main]
use libfuzzer_sys::fuzz_target;
#[allow(unused_imports)]
use napple_fuzz::*;

fuzz_target!(|data: &[u8]| {
    // Input = NCZ file; the first 0x4000 bytes are the raw (encrypted) NCA header block, so
    // the header trick applies to the decrypted form: re-encrypt before opening.
    let mut d = data.to_vec();
    if d.len() >= 0x4000 {
        let e = encrypt_nca_header(&d);
        d[..0xC00].copy_from_slice(&e[..0xC00]);
    }
    if let Ok(s) = napple_core::infrastructure::ncz::NczSource::open(src(&d)) {
        touch(&s);
        let mut b = [0u8; 4096];
        let _ = s.read_at(
            s.size().saturating_sub(4096),
            &mut b[..(s.size().min(4096) as usize)],
        );
        let ks = keys();
        if let Ok(n) = napple_core::infrastructure::nca::Nca::open(s, &ks, &Default::default()) {
            for sec in &n.sections {
                if let Ok(x) = n.section(sec.index) {
                    touch(&x);
                }
            }
        }
    }
});
