# MK8D ahead-of-time AArch32 -> AArch64 recompiler (branch claude/mk8d-recompiler-2026-10-25-titles, Mac)

## What it is
`runtime/src/a32jit/` (C, shares the interpreter's decoder `src/a32/a32_decode_*.c`) lifts the ARM / Thumb-2 code of the AArch32 modules to AArch64 **assembly**; clang builds it into an ordinary dylib (`<data dir>/modules/a32jit.dylib` + `a32jit.manifest`). Nothing is generated at run time, so the image can be signed like any dylib (iOS forbids JIT). Mixed mode: the runtime interprets anything without a lifted block and every instruction the lifter does not lift natively calls the interpreter for exactly that instruction, so the image never needs to be complete to be correct.

Design (all in `a32lift.c`):
- guest r0..r14 pinned in w0..w14, guest N Z C V in the host NZCV (A64 add/sub/adc/sbc flags are ARM's; logical S ops use `rmif`, FEAT_FlagM = Armv8.4, Apple A13+/M1+), x19 instruction budget, x20 guest window base, x21 `A32Cpu*`, x22 hash table.
- Discovery: ELF symbols, every relocated slot pointing into text, init arrays, recursive walk of branch targets / call return sites / Thumb `tbb`/`tbh` tables / ARM `add pc,pc` branch tables, a scan for function prologues, plus optional profile-guided extra entry points (`<modules>/a32jit.seeds`, written by `NAPPLE_A32_JITLOG=<file>` runs). IT blocks are tracked statically.
- Blocks chain with direct branches; indirect branches / returns hash `(guest address | thumb)` -> block (`_a32jit_ind`); a branch into the host-import stub range (0xF00xxxxx) calls the host function in place and continues at lr; a miss leaves to the dispatcher (`a32jit.c`), which interprets until the pc is in a block again. A budget check at loop heads / call targets keeps slices bounded.
- Natively lifted: data processing (all forms except register-shifted + S-flag logical), mul/mla/umull/smull/..., div, clz/rbit/rev*, sxt/uxt(+add), ubfx/sbfx/bfi/bfc, movw/movt, single/double loads and stores (all addressing modes), ldm/stm/push/pop, ldrd/strd, ldrex/strex family (host exclusive monitor), dmb/dsb (as `dmb ish`), clrex, tpidruro, all branches (b/bl/blx/bx/cbz/tbb/tbh/ldr pc/pop pc/mov pc). Fallback (interpreter, per instruction): VFP, NEON, saturating/parallel arithmetic, halfword multiplies with Q, svc, cp15 other than TLS, register-shifted logical S, ldrexd corner cases.

## Tools
- `tools/build_a32jit.sh <data dir>`: builds the lifter, lifts, assembles in parallel (about 18 s on 8+ cores for MK8D, 82 MB dylib, 4.4M instructions, 605k blocks), links, installs into `<data dir>/modules`.
- `napple rewrite a32 <modules dir> [-o work]` (crates/napple-rewrite/src/a32.rs; build.rs compiles the same C lifter into the crate). `NAPPLE_A32_RECOMPILE=1 napple build ...` runs it after the ELF32 modules are written (crates/napple-build/src/tables32.rs `recompile_if_requested`, one call in pipeline.rs).
- Runtime: `NAPPLE_A32_JIT=0` disables it, `NAPPLE_A32_JITSTATS=1|2` (counters / profile of interpreted pcs), `NAPPLE_A32_JITLOG=<file>` (entry points the static walk missed). `A32LIFT_ONLY=main,sdk` and `A32LIFT_SEEDS=<file>` for the lifter.

## Verified (all on this Mac)
Differential tests against the interpreter (itself fuzzed against Unicorn by tests/a32/fuzz.py), same random register/flag/memory state, compare r0-r14, pc, NZCVQ, GE, VFP state, 64 KiB arena. Samples where the interpreter rejects, touches memory outside the arena, or hits an unaligned exclusive are skipped:
| test | samples compared | lifted natively | mismatches |
|---|---|---|---|
| `difftest` ARM families (dp, mul, mull, ldst, ldm, extra loads, media, branch, halfmul, movw, sync, ...) | 58,410 | 39,247 | 0 |
| `difftest --mode thumb` (random 16/32-bit) | 58,704 | 55,016 | 0 |
| `difftest --from main/sdk/subsdk4.patched.bin` (real MK8D code distribution, ARM) | 387,942 | 352,111 | 0 |
| `difftest --mode thumb --from sdk.patched.bin` | 102,481 | 101,342 | 0 |
| `ittest` (random IT blocks of 1-4 instructions) | 14,412 | n/a (whole blocks) | 0 |
Per-family counts are printed by `difftest`. Mismatches found and fixed while building it: TEQ result temp clobbering the pc-relative shifter source (found by the random ARM fuzz), ldrd with rt == rt2 (assembler rejects), UNDEF handling under a false condition, dispatcher hash export.
Micro-benchmark (`bench`, tight integer loops): interpreter 242 / 249 M guest instructions/s (ARM / Thumb), lifted 9,500 / 11,000 M/s (about 40x).
Regressions: `cargo test --workspace`, `make -C runtime test test-services`, `make -C gfx test` pass; LAS and SMO boot 45 s headless, exit 0, 0 CRASH (LAS 2639 frames, SMO 1952).

## MK8D, measured (headless `napple-runtime <data dir> --headless`, NOT played)
The game does not reach a steady title screen in this checkout: a worker thread calls a method on a NULL singleton (`ldr r0,[r0,#0x18]` at main+0x822994 / main+0x562d00, `this` = 0 + 0x3e74; reached through the thread entry at main+0x766f28), with the interpreter as well (2 of 3 runs, at about 19-23 s) and with the lifted code (20 of 20 runs). It is a game start-up ordering problem (service or kernel scheduling emulation, outside this assignment), so steady-state fps could not be measured. What was measured is the same fixed point of the boot, time to the first guest fault and gfx frames drawn until then:
| | interpreter | lifted (mixed mode) |
|---|---|---|
| wall time to the fault, runs | 19.4, 18.7, 18.9, 19.9, 19.1, 20.6, 23.4, 18.9 s | min 2.6, median 2.8, max 4.6 s (20 runs) |
| gfx frames drawn before it | 138-166 (about 7-8 fps average over the boot) | 175-182 (about 60-70 fps average over the boot, mostly loading screens) |
So boot-to-title is about 7x faster end to end (includes the unchanged host side: shaders, file IO, audio). The 30 fps goal on the title screen is not demonstrated. Remaining interpreted work seen by `NAPPLE_A32_JITSTATS=2`: VFP / NEON loops (skinning, matrices; main+0xc08fa0, main+0x84708, sdk+0x43f54 string code was fixed by profile seeds), `svc`, and the HLE import calls (about 600k/s, now handled in place).

## Known issues / risks
- MK8D NULL-singleton fault above (pre-existing, both cores).
- Rare wild-pc crash seen only with lifted code (4 of about 35 early runs with older images, 1 of 5 with the current one, 0 of the last 20): a return to a garbage Thumb address in the main thread. Not reproduced under test; memory ordering (barriers are kept as `dmb ish`, exclusives native) was the first suspect and fixed; if it returns, bisect with `A32LIFT_ONLY`.
- VFP/NEON are not lifted (interpreter per instruction, about 70 host instructions of state sync each): next lever for speed. Exception flag bits of FPSCR would not be maintained by a native VFP lift.
- Faults inside lifted code report stale guest registers (they live in host registers x0-x14 at that moment; the runtime crash dump prints them as x0..x14).
- The image embeds absolute guest addresses; the manifest check refuses an image built for different module bases.
- Needs Armv8.4 FlagM (`rmif`): Apple A13 / M1 and later. For older iOS devices add an `mrs/msr` fallback in `lift_dp`.

## iOS packaging (not done, as assigned)
The dylib is plain Mach-O with no undefined symbols: put `a32jit.dylib` + `a32jit.manifest` in the app's data directory / Frameworks, sign it with the app, and `dlopen` it (a32jit.c does; the iOS guest window base differs only if module bases change, which the manifest check covers). Size 82 MB (hash table 33 MB of it as relocatable pointers); a compact table of 32-bit offsets would shrink it. No `MAP_JIT` or runtime code writing anywhere.

## Files
New: `runtime/src/a32jit/{a32lift.c,a32lift.h,a32lift_main.c,a32jit.c,a32jit.h}`, `runtime/tests/a32jit/{difftest.c,ittest.c,bench.c}`, `tools/build_a32jit.sh`, `crates/napple-rewrite/{build.rs,src/a32.rs}`.
Edited (additive hooks): `runtime/src/a32/a32_proc.c` (include, `run_slice` uses `a32jit_run` when the image is active, `a32jit_init` after relocation), `runtime/Makefile` (`src/a32jit/a32jit.c` added to SRC), `crates/napple-rewrite/{Cargo.toml,src/lib.rs,src/cli.rs}`, `Cargo.lock` (cc as build-dependency, already in the lock), `crates/napple-build/src/{tables32.rs,pipeline.rs}` (one call).
