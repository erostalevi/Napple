/* a32lift: ahead-of-time AArch32 -> AArch64 lifter.
 *
 * Takes the guest memory image of the 32-bit modules (the same bytes the runtime maps: runtime/src/a32/a32_module.c) and emits AArch64
 * assembly. The assembly is built by clang into a dylib (no runtime code generation, so the same image can be signed and shipped on iOS).
 * See docs/archive/changes/2026-10-25-mk8d-recompiler.md for the design: guest r0..r14 are pinned in w0..w14, the guest flags in the host NZCV, the
 * guest window base in x20; blocks are chained by direct branches and indirect branches go through a hash table; every instruction the lifter
 * does not handle calls the interpreter for exactly that instruction (mixed mode). */
#pragma once
#include <stdint.h>
#include <stdio.h>

typedef struct {
    const char *name;
    uint32_t base;             /* guest address the module is mapped at */
    uint32_t text_end;         /* module-relative end of the code */
    uint32_t seed_lo, seed_hi; /* index range into LiftInput.seeds that belongs to this module (unused by the tool, informational) */
} LiftModule;

typedef struct {
    const uint8_t *mem; /* host address of guest address 0 */
    LiftModule *mods;
    unsigned nmods;
    uint32_t *seeds;
    unsigned nseeds; /* entry points: guest address, bit 0 = Thumb */
    int each_entry;  /* test mode: every instruction is an entry of its own (differential tests) */
    uint8_t *seed_fb;
    uint32_t fb_base, fb_stride; /* test mode output: seed_fb[(addr - fb_base) / fb_stride] = 1 when that instruction fell back to the interpreter */
} LiftInput;

typedef struct {
    uint64_t entries, insns, lifted, fallback, bytes_asm;
    uint64_t fb_by_op[64];
} LiftStats;

/* Writes `<prefix>.NN.S` (code chunks), `<prefix>.tab.S` (hash table + common stubs). Returns the number of files written (names in
 * out_files, malloc'd), or -1. */
int a32lift_run(const LiftInput *in, const char *prefix, char ***out_files, LiftStats *st);
