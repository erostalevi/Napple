# Add a Maxwell shader instruction or op

## Goal
Make the in-process Maxwell (SM 5.x) to Metal Shading Language translator handle an instruction it refuses today. The symptom is a log line
`[napple-gfx] warn: shader <hash> (stage <n>) could not be translated: unknown instruction <16 hex digits> at 0x<pc>` (or another message set by `fail()`), draws that use the
program are skipped (`skipped draws` in the `[napple-gfx] stats:` line), and the picture misses geometry or effects.
Background: [gfx.md](../architecture/gfx.md), "Decision 1" (why a native translator) and the binding contract of the generated MSL in `gfx/src/shader/maxwell.h`.

## How the translator is organised
- Entry point: `maxwell_translate(blob, max_size, stage, key)` in `gfx/src/shader/maxwell_translate.c` returns a `ShaderInfo` (`maxwell.h`) with the MSL text in `msl`, or `msl == NULL` and a
  message in `error`. Called from `mtl_shader.m: build_shader`, which also caches the result on disk by `TRANSLATOR_VERSION`.
- The blob is the NVN header (0x30 bytes), the SPH (0x50 bytes, graphics stages) and instructions in groups of four 64-bit words: one scheduling word and three instructions. Compute programs have only code.
- `maxwell_translate` finds the end of the program, collects branch targets, then calls `emit_insn(Ctx *c, uint64_t w, uint32_t pc)` for every instruction word. Registers become `uint rN` variables,
  floats use `as_type<float>()`; programs with branches become a `switch (pc)` state machine with SSY/PBK/PCNT stacks.
- `emit_insn` is **one long `if / else if` chain** of `OPM(value, mask)` tests on the instruction word (`#define OPM(val, mask) ((w & mask) == val)`), one branch per instruction
  with a comment naming it (`// ISCADD`, `// VMNMX`...). The fixed fields are read at the top: `d = bits(w, 0, 8)`, `a = bits(w, 8, 8)`, predicate in bits 16-19. A guarded instruction is wrapped in `if (P) { ... }` by the caller.
- Operand helpers: `reg(c, r, buf)` / `R(r)`, `pred`/`P(p, n)`, `cbuf_ref(c, idx, off, dyn, buf)`, `operand_b(c, w, float_imm, buf)` / `B(float_imm)` (register, constant-buffer or immediate form chosen from the opcode class),
  `set_reg(c, d, expr)` (assign and mark the register used), `sp(&c->body, fmt, ...)` (append MSL), `bits`/`sbits`, `fail(c, fmt, ...)` (refuse the shader with a message).

## Files to touch
| Path | What |
|---|---|
| `gfx/src/shader/maxwell_translate.c` | The new `else if (OPM(...))` branch in `emit_insn`; new `Ctx` flags if the op needs a prelude in the generated source. |
| `gfx/src/shader/maxwell.h` | Only if the generated MSL needs a new binding or `ShaderInfo`/`ShaderKey` field (then also `mtl_shader.m` and `mtl_replay.m`, which bind them). |
| `gfx/src/mtl_shader.m` | `#define TRANSLATOR_VERSION` (currently 8): **bump it when the generated MSL changes**, which invalidates the disk caches (`pipelines-v<N>.metalar`, per-shader caches). |
| `gfx/tests/t_xlate.m` | The translator test: assembler helpers (`i_*`), `XL_OK` / `XL_REFUSE` / `HAS` macros, numeric execution tests (`run_fs_u4`, macOS only). |
| `gfx/tests/nvn_test.h` | Shared helpers `ins_exit`, `ins_mov32i`, `ins_ald`, `ins_ast`, `build_shader`, `PT_BITS`. |
| `gfx/tests/t_xlate_linux.sh` | Builds and runs `t_xlate` without Metal. |

There is no corpus directory in the repository: shaders extracted from games are Nintendo/third-party data and stay outside it (see "How to test").

## Steps
1. **Get the word.** The failing instruction is printed in the warning. To keep the whole program for study set `NAPPLE_SHADER_DUMP=<existing dir>`: untranslatable programs are written as
   `<hash>_s<stage>.bin` there, and `NAPPLE_SHADER_DUMP_HASH=<hash hex>` dumps one specific program even if it translates (`mtl_shader.m: build_shader`). The file is the Maxwell code with its header;
   `NAPPLE_SAVE_MSL=1` makes the translator annotate the generated MSL with `// pc <addr>: <word>` for each instruction.
2. **Identify the instruction** from the opcode bits and a reliable reference (public Maxwell ISA descriptions, or existing translators; existing branches cite their sources, e.g. the VMNMX field layout comes from
   Ryujinx's decoder and is credited in the comment). Write down the opcode value and mask, the operand fields, and the modifiers.
3. **Add the branch** to the chain in `emit_insn` **before any broader mask that would match first** (the first matching `OPM` wins). Use `OPM(0x....ull, 0x....ull)` with a comment naming the op.
   - Read fields with `bits(w, lo, n)`; take operand B with `B(false)` (integer) or `B(true)` (float immediate form); the result register with `d`.
   - Emit MSL with `sp(&c->body, ...)` or, for a single-expression result, `sprintf(t, "...")` then `set_reg(c, d, t)`.
   - If you write a destination register yourself, set `c->reg_used[d] = 1` and, when it was last loaded from a constant buffer, `c->reg_cb[d] = -1` (what `set_reg` does).
   - Use `{ ... }` blocks for temporaries, because the code lands inside the `if (P) {` guard and inside the `switch`.
   - Refuse cleanly what you do not implement: `fail(c, "<op>.<modifier> is not supported")`. A translation failure only skips the draws that use the shader; a wrong translation draws garbage.
4. **Carry state if needed.** Condition-code carries use `c->uses_cc` and a `cc` variable (see ISCADD, IMAD, LEA); new generated helpers need a `Ctx` flag and a line in the program assembly after the `// MSL source` marker.
5. **Bump `TRANSLATOR_VERSION`** in `mtl_shader.m`.
6. **Add tests** (below) and a note in the graphics notes if the op was found in a game.

## Worked example
`VMNMX` (video min/max) is a complete, recent case; the branch is in `maxwell_translate.c` (search `// VMNMX`), its test is in `gfx/tests/t_xlate.m`.
- Found in Animal Crossing: fragment shader `23e43e94e45bb1d5` at 0x3e8 holds `0x3b34056060470100`, and the old log read "unknown instruction 3b34056060470100" ([symptoms.md](../symptoms.md), Rendering).
- The branch: `else if (OPM(0x3a00000000000000ull, 0xfe00000000000000ull)) {  // VMNMX ...` reads A bits 36..38 and sign 48, B bits 28..30 and sign 49, B from a register when bit 50 else an immediate 16 at bit 20, MX bit 56,
  D-format 54, SAT 55, the second video op 51..53, CC 47 (`fail` for `.CC`); it selects bytes or halves, does `max`/`min` in 64 bits and merges with `insert_bits(...)` or `min`/`max` with Rc, then `r<d> = res`.
- The tests in `t_xlate.m`:
  1. an assembler helper, `i_vmnmx(d, a, b, c, asel, asg, bsel, bsg, bvid, mx, vop, dfmt, sat)`, builds a word from the fields (`| PT_BITS` makes the predicate PT);
  2. a golden check that the real word translates and the MSL has the expected text:
     ```c
     XL_OK(vm, STAGE_FRAGMENT, z, 0xf, "VMNMX from Animal Crossing", 0x3b34056060470100ull, ins_exit());
     CHECK(HAS(vm, "long vr = max(va, vb);") && HAS(vm, "res = max(res, vc);") && HAS(vm, "r0 = res;"), "VMNMX MAX, U32: three-way unsigned maximum");
     ```
     `XL_OK` translates with `xl_ins` and (on macOS) compiles the MSL with Metal; `XL_REFUSE(var, stage, key, omap, why, label, ...)` asserts that an unsupported form is refused with a given message;
  3. under `#ifdef __APPLE__`, numeric execution: `run_fs_u4(vm->msl, o, 0)` runs the fragment shader on a 1x1 `RGBA32Uint` target and compares registers r0..r3 (mov32i the inputs, run the op, `EXIT`).
Another model for a smaller op: `// ISCADD32I` (a two-line branch) and `// IMAD` (several encodings, honest comment "Bit layout from memory; no real word in the LAS or SMO corpora").

## How to test
- **Linux, no Metal:** `sh gfx/tests/t_xlate_linux.sh` compiles `tests/t_xlate.m` as C together with `src/shader/maxwell_translate.c` and runs every translation and text assertion; Metal compile checks are skipped and counted. Measured on a Linux box:
  `t_xlate: 50 Metal compile checks skipped (no Metal on this host)` then `t_xlate: ok`. This proves the text, not that the MSL compiles or is numerically right.
- **macOS:** `make -C gfx test` builds and runs `t_xlate` among all `tests/t_*` programs. There every `XL_OK` also compiles the MSL with Metal (`MTLLanguageVersion3_0`, `compiles()`), and `run_fs_u4` executes shaders and compares numbers.
  To run only this test: `make -C gfx build/tests/t_xlate && gfx/build/tests/t_xlate`.
- **Corpus of real shaders (kept outside the repository):** set `NAPPLE_SHADER_CORPUS=<dir>` to a directory of extracted shaders, each `<name>.bin` (code) with a `<name>.ctl` (control section, whose word at +0x700 is the code length),
  stage chosen from the file name (`_ps.` fragment, `_cs.` compute, otherwise vertex). `t_xlate` then prints `corpus: N shaders, M translated, K compile with Metal` and fails below 95 % translated or if any translated shader fails the Metal compile.
  The titles' pages quote results of this mode as corpus guards (`t_las`, `t_smo`; [titles/01006BB00C6F0000.md](../titles/01006BB00C6F0000.md), [titles/0100000000010000.md](../titles/0100000000010000.md)); no file of those names exists in `gfx/tests/`.
  Produce the files with `NAPPLE_SHADER_DUMP` while running a title you own; do not commit them.
- **In a game:** run the title and check that the `could not be translated` line is gone and the picture is right. `NAPPLE_GFX_MATH=safe|relaxed|fast` selects the MSL math mode (default `safe`); `NAPPLE_SHADER_PROBE`
  (see `mtl_shader.m`, `mtl_replay.m`) shows chosen registers as the fragment colour after one instruction, and `NAPPLE_DEBUG_SKIP_VS` bisects vertex programs ([debugging.md](debugging.md)).

## Common pitfalls
- **First match wins.** A broad `OPM` placed earlier silently swallows your new opcode; place narrow masks first, and run the existing tests (they pin the older ops).
- **NaN semantics.** Maxwell's ordered not-equal is false when either operand is NaN, while MSL `!=` is true: the translator emits `(a < b) || (a > b)` for FSETP/FSET/FCMP NE and `precise::fmin/fmax` for FMNMX
  ([symptoms.md](../symptoms.md), Rendering).
- **Bounds on constant buffers.** Register-indexed constant-buffer reads past the bound size return 0 on hardware; use `cbuf_ref(..., dyn, ...)`, which emits `cbr(cb, size, off)`, never a raw index (SMO's garbage bones).
- **Layouts "from memory".** Several branches say so in their comment and have no real word in the corpora they were checked against. Keep that honesty in new comments, and make the unverified forms `fail()` instead of guessing.
- **Carries and 64-bit math.** `.CC`/`.X` forms need the `cc` variable; if you do not model them, refuse them.
- **Cache invalidation.** Forgetting `TRANSLATOR_VERSION` leaves old MSL in `pipelines-v<N>.metalar` and the per-shader cache; the symptom is "my fix does nothing" on a machine that already ran the title.
- **Metal limits.** Threadgroup memory above 32 KB is refused (`shared memory of %u bytes exceeds Metal's 32 KB threadgroup limit`); there is no geometry stage (OUT is refused) and no f64 (DMNMX is refused); surface instructions are translated to texture reads, writes and atomics (see gfx.md section 11; INC, DEC, EXCH, CAS and 64-bit / float surface atomics are refused).
- **Stage-specific state.** Fragment shaders depend on the vertex shader's outputs through `ShaderKey.vs_out_mask`; a change that alters when varyings are read can change which variants are built (pipeline stutter, [symptoms.md](../symptoms.md)).
