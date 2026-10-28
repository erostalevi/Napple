#![no_main]
use libfuzzer_sys::fuzz_target;
#[allow(unused_imports)]
use napple_fuzz::*;

fuzz_target!(|data: &[u8]| {
    // Input = NSO0 file bytes -> parser -> module image -> relocations -> rewriter.
    use napple_rewrite::{RewriteOptions, image::ModuleImage, relocs, rewrite_module};
    let Ok(nso) = napple_core::infrastructure::nso::Nso::parse(data) else {
        return;
    };
    let Ok(img) = ModuleImage::from_nso(&nso) else {
        return;
    };
    let tags = img.dynamic_tags();
    let _ = img.function_starts();
    let _ = relocs::collect(
        &img.bytes,
        &tags,
        nso.data.mem_offset as u64,
        img.layout.image_len as u64,
    );
    let _ = rewrite_module(&nso, "00", &RewriteOptions::default());
});
