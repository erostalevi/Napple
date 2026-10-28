#![no_main]
use libfuzzer_sys::fuzz_target;
#[allow(unused_imports)]
use napple_fuzz::*;

fuzz_target!(|data: &[u8]| {
    let _ = napple_build::nro::parse_nro(data);
    let _ = napple_build::macho::layout_from_ntb1(data);
    let _ = napple_build::sysdata::decode_bfttf(data);
});
