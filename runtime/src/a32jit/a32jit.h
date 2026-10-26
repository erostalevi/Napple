/* a32jit: run-time side of the ahead-of-time AArch32 -> AArch64 recompiler (the lifter is a32lift.c, built by tools/build_a32jit.sh).
 * The lifted code lives in <data dir>/modules/a32jit.dylib, an ordinary signed Mach-O: nothing is generated at run time. Guest code without a
 * lifted block (HLE stubs, anything the lifter never reached) is interpreted by a32_run, one instruction at a time, until the pc is in a block again. */
#pragma once
#include "../a32/a32.h"
#include "../a32/a32_module.h"

/* Loads the image next to the modules and checks that it was lifted for exactly these module bases. Returns 1 when lifted code is active. */
int a32jit_init(const char *modules_dir, const A32Window *w);
int a32jit_active(void);
/* Like a32_run(c, budget): runs lifted code with the interpreter as fallback. A32_EXIT_BUDGET after about `budget` guest instructions. */
A32Exit a32jit_run(A32Cpu *c, uint64_t budget);
