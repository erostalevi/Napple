# 2026-11-02 leftovers-a32-viewport-docs (batch 2026-11-02-r5, Mac)

Branch `claude/leftovers-a32-viewport-docs-2026-11-02-r5` (base `orchestrate/2026-11-02-r5` = `2a5dab5`). Three leftovers of round 4: the AArch32
interpreter's `lda` / `stl`, per-viewport swizzles, and the docs checker's orphans and stale support tables.

## 1. Interpreter `lda` / `stl` (`runtime/src/a32/a32_exec.c`)
- `lda` / `ldab` / `ldah` and `stl` / `stlb` / `stlh` (ARM and Thumb) are now seq_cst atomic loads and stores (`exec_ldst_acq`). They were a plain
  access with an acquire fence after the load and a release fence before the store, which is acquire / release but not RCsc: `stl` then `lda` could
  reorder. AArch32's load-acquire / store-release forbid that. Same semantics as the lifter's `ldar` / `stlr` (r4).
- An unaligned address raises the interpreter's alignment exit (`A32_EXIT_ALIGN`, `fault_addr`), as the guest's core does for these instructions
  whatever SCTLR.A says, and as the exclusives already did.
- `runtime/tests/a32jit/difftest.c`: the r4 skip of unaligned `lda` / `stl` is gone. Those samples now end in the interpreter's alignment exit, so
  they are not compared (the same path as the unaligned exclusives). The ARM run compares 11587 samples instead of 11590.
- Only the debug interpreter build (`NAPPLE_A32_INTERP`) and the tests run this code: the shipped runtime has no AArch32 interpreter
  (`check_a32_no_interp.sh`).

## 2. Per-viewport swizzles (`gfx/`)
- `ShaderKey.vp_swizzle_vp[15]` (in place of `vp_pad`) and `pipe_key.vp_swizzle_vp[15]` (+ `pad_vp2`) carry the swizzles of viewports 1-15, in the
  encoding of `vp_swizzle` (XOR `VP_SWIZZLE_IDENTITY`, 0 = identity). `vp_swizzle_key` (`mtl_replay.m`) fills all 16 from `S.vp_swz`.
- New `maxwell_vp_swizzle_msl` (`maxwell_translate.c`, declared in `maxwell.h`) writes the epilogue statement for both the translator and the
  geometry pass-through:
  - when every viewport has the same swizzle, or the program writes no viewport index, it emits the static swizzle as before (nothing for the
    identity, so the MSL of every program without swizzles is unchanged);
  - otherwise it emits a table of the 16 swizzles, indexed by the viewport index (clamped to 15), and a run-time select of each component.
- Viewport index source:
  - vertex and tess eval: the program's store to attribute 0x68 (`vsys[2]`); the translator tracks it as `writes_vp_index`;
  - geometry pass-through: slot 0 `.z` of the output record (0 when the geometry program stores none: `gso` starts at `(0, 0, 0, 1)`).
- The pass-through function cache is keyed by the full 16-entry table (`NSData` key). `TRANSLATOR_VERSION` 19.
- Capture-only passes (vertex or tess eval into stage-out records or transform feedback, rasterization off) still apply no swizzle, and the capture
  key keeps all 16 at 0. Decision: the viewport swizzle belongs to the viewport transform, which comes after transform feedback on the hardware, so
  captured values must be unswizzled; with a geometry program the swizzle applies once, in the pass-through that feeds the rasterizer.
- Not changed: Metal still gets one viewport (`setViewport:` with viewport 0's rectangle). The viewport index now selects the swizzle, but not the
  rectangle: multi-viewport rasterization (`[[viewport_array_index]]` + `setViewports:count:`) is not implemented. No title in the corpus is known
  to write the viewport index from a vertex program.
- `gfx/tests/t_vp_swizzle.c` cases 5-10, viewport 0 identity and viewport 1 x = -x (or x <-> y):
  - a vertex program that writes viewport index 1 gets viewport 1's swizzle (right half, then a horizontal half);
  - the same draw with a program that writes no index gets viewport 0's (left half);
  - index 0 written explicitly takes viewport 0's (x = -x: right half);
  - a geometry program that forwards the triangle and writes index 1 gets viewport 1's in the pass-through (right half, then a horizontal half).

## 3. Docs
- `docs/process/archive/README.md` (new): the orchestration and integrator files of rounds r1-r4 per round, with each item's goal. Linked from
  `docs/index.md`. The docs checker's 38 orphans under `docs/process/archive` are gone.
- `docs/archive/changes/README.md`: a section "Unattended rounds" with every change note of batch b7 that the README did not list, and of rounds
  r1-r5, one line each. `docs/index.md` links that section instead of the r4 nested list. The two r4 notes the checker reported (gfx-views-raster,
  ipc-reference-audit) are in it.
- Support tables regenerated: `python3 -I tools/support/gen_support.py --refresh-snapshot .claude/skills/percent-analysis/history`
  (snapshot 2026-11-02-8389280, 98.9%; it was 2026-10-26, 91.5%), and the matrix from `compat/matrix.json` (last compat run 2026-10-28).
  `--refresh-snapshot` without an argument looks for the history at `../.claude/`, the old nested layout; in this flat checkout it has to be given.
- `python3 tools/check_docs.py`: `checked 241 Markdown files: OK` (was 42 problems).

## Verified here (Mac)
| Check | Result |
|---|---|
| `make -C runtime GFX=napple test-a32` | pass: excltest 47/47; difftest ARM 11587 / Thumb 11743 compared, 0 mismatches; inline words 1032723 samples, 0 forms failed; progtest ok; no interpreter in the runtime |
| difftest, single `lda` / `stl` words (`--word`, 2000 samples each) | ARM `lda` / `ldah`: 931 / 686 samples end in the alignment exit, the rest (752 / 800) compared with 0 mismatches; byte forms 931 compared; `stl*` the same; Thumb `lda` / `ldah` / `stl` / `stlh` 2000 compared each, 0 mismatches |
| difftest `--family sync`, seeds 2 / 3 / 7, ARM and Thumb | 0 mismatches |
| `make -C gfx test` | ALL TESTS PASSED (the four known-open SKIPs: `t_clear_ds`, `t_clear_tex` x2, `t_feedback`); `t_vp_swizzle: ok`, `t_obj_canary: ok`, `t_xlate: ok` |
| `MTL_DEBUG_LAYER=1 gfx/build/tests/t_vp_swizzle` | 10 cases ok, 0 skipped, no validation error |
| LAS corpus (`NAPPLE_SHADER_CORPUS=/Users/neto/NappleWork/shader-corpus/las gfx/build/tests/t_xlate`) | 5767 programs, 5767 translated, 5767 compile with Metal |
| `python3 tools/check_docs.py` | OK, 241 files |
| `python3 -I tools/support/gen_support.py --check` | support tables: OK |

### Four-title boots (`MTL_DEBUG_LAYER=1`)
Settings as r4's gfx-views-raster regression: headless, 60 s (ACNH 90 s), A at 25 / 40 / 55 / 70 s, screenshots at frames 1000 / 2000, a cold
private save root per title, one run at a time under the run lock. Logs: `/Users/neto/NappleWork/logs/leftovers-r5-head{,2}-<titleid>/`.
Head only: the new code runs only when a title sets viewport swizzles, and none does (no `viewport swizzles in use` line in any log), so the
comparison is against r4's head logs (`gfxraster-reg-head-*`).

| Title | r4 head frames | r5 run 1 (cold Metal compiler cache) | r5 run 2 (warm) |
|---|---|---|---|
| ACNH | 2682 | 2612 | 2681 |
| LAS | 3529 | 3552 | 3576 |
| SMO | 3227 (r4 base 2997) | 2603 | 3107 |
| MK8D | 3565 | 3440 | 3565 |

- Every run exits 0, 0 skipped draws, 0 warning lines, no Metal validation error (only the "Validation Enabled" banner).
- Run 1 compiled every shader cold (Metal compile 11-24 ms on average; r4's runs show 0.0-0.1 ms, a warm system compiler cache), and another
  session's runtime was running at the time (load average 5). Run 2, with the cache warm, is within noise of r4.
- Screenshots against r4's head: SMO and MK8D 0.00-0.07 mean difference; ACNH 3.3 / 4.1 (the same Nook Inc. dialog, idle animation); LAS
  f-1000 13.3 (the animated storm intro at another moment, f-2000 0.00). Checked side by side.

## Files
`runtime/src/a32/a32_exec.c`, `runtime/tests/a32jit/difftest.c`; `gfx/src/shader/maxwell.h`, `gfx/src/shader/maxwell_translate.c`,
`gfx/src/mtl_replay.m`, `gfx/src/mtl_shader.m`, `gfx/src/mtl_priv.h` (pipe_key fields appended), `gfx/tests/t_vp_swizzle.c`;
`docs/index.md`, `docs/archive/changes/README.md`, `docs/process/archive/README.md` (new), `docs/reference/support-matrix.md`,
`docs/reference/feature-support.md`, `docs/reference/support-data.json`, `tools/support/data/coverage-snapshot.json`, this note.
