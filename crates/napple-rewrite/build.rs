//! Builds the AArch32 -> AArch64 lifter (C, shared with the runtime's decoder: runtime/src/a32jit and runtime/src/a32) into the crate, so that
//! `napple rewrite a32` needs no separate tool. The lifter only reads the module images; it writes assembly that `a32.rs` assembles with clang.
fn main() {
    let rt = "../../runtime/src";
    let files = [
        "a32jit/a32lift.c",
        "a32jit/a32lift_main.c",
        "a32/a32_module.c",
        "a32/a32_decode_arm.c",
        "a32/a32_decode_thumb.c",
    ];
    let mut b = cc::Build::new();
    for f in files {
        b.file(format!("{rt}/{f}"));
        println!("cargo:rerun-if-changed={rt}/{f}");
    }
    println!("cargo:rerun-if-changed={rt}/a32jit/a32lift.h");
    b.define("A32LIFT_NO_MAIN", None).include(format!("{rt}/a32")).flag_if_supported("-std=gnu11").opt_level(2).warnings(false).compile("a32lift");
}
