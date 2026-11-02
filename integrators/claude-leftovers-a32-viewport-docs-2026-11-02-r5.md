---
branch: claude/leftovers-a32-viewport-docs-2026-11-02-r5
orchestration: leftovers-a32-viewport-docs
base: 2a5dab5 (orchestrate/2026-11-02-r5)
head: fdb92d1 (code and generated docs; the commit on top of it adds the change note and this file)
env: mac
date: 2026-11-02
areas: [runtime/a32, gfx, docs]
status: ready
---

# claude/leftovers-a32-viewport-docs-2026-11-02-r5: interpreter lda/stl ordering + alignment, per-viewport swizzles, docs orphans

## Summary
- **Interpreter `lda` / `stl`** (`a32_exec.c`): seq_cst atomic loads / stores (RCsc, like the lifter's `ldar` / `stlr`) instead of a plain access
  with a one-sided fence. An unaligned address raises `A32_EXIT_ALIGN`. The r4 difftest skip is removed; `test-a32` green.
- **Per-viewport swizzles:**
  - viewports 1-15 carry their own swizzle (`ShaderKey.vp_swizzle_vp[15]`, `pipe_key.vp_swizzle_vp[15]`);
  - a vertex / tess eval program that writes the viewport index (0x68), or a geometry program that does (pass-through, slot 0 `.z`), selects its
    viewport's swizzle at run time from a 16-entry table;
  - the MSL is unchanged when every viewport has the same swizzle or the program writes no index;
  - `t_vp_swizzle` cases 5-10 (viewport 1 != viewport 0, vertex and geometry);
  - capture-only passes: decided to keep no swizzle (it belongs to the viewport transform, after transform feedback).
- **Docs:**
  - new `docs/process/archive/README.md` indexes the 38 orchestration / integrator files of r1-r4;
  - `docs/archive/changes/README.md` lists every b7 and r1-r5 change note;
  - `docs/index.md` links both;
  - support tables regenerated with the newest coverage snapshot;
  - `check_docs.py` OK (was 42 problems).
- Not done: multi-viewport rasterization (Metal still gets viewport 0's rectangle only; see Findings). It was not in the assignment.

## Files
`git diff --stat 2a5dab5..HEAD`:
- Runtime: `runtime/src/a32/a32_exec.c` (`exec_ldst_acq`, `ld_sub` without the fence), `runtime/tests/a32jit/difftest.c` (one line removed).
- gfx:
  - `gfx/src/shader/maxwell.h`: `ShaderKey.vp_swizzle_vp[15]` replaces `vp_pad` (the struct grows by 28 bytes); `maxwell_vp_swizzle_msl` prototype.
  - `gfx/src/shader/maxwell_translate.c`: `writes_vp_index`, `maxwell_vp_swizzle_msl`, the vertex / tess eval epilogue.
  - `gfx/src/mtl_replay.m`: `vp_swizzle_key(pipe_key *)`.
  - `gfx/src/mtl_shader.m`: the stage keys copy `vp_swizzle_vp`; `gs_passthrough_fn` takes the pipe key, its cache key is `NSData`; the capture-key
    normalisation clears `vp_swizzle_vp`; `TRANSLATOR_VERSION` 19.
  - `gfx/src/mtl_priv.h`: appended `pipe_key.vp_swizzle_vp[15]`, `pad_vp2`.
  - `gfx/tests/t_vp_swizzle.c`.
- Docs:
  - `docs/index.md` (links), `docs/archive/changes/README.md` (section appended), `docs/process/archive/README.md` (new);
  - generated: `docs/reference/support-matrix.md`, `feature-support.md`, `support-data.json`, and its input `tools/support/data/coverage-snapshot.json`;
  - `docs/archive/changes/2026-11-02-claude-leftovers-a32-viewport-docs-2026-11-02-r5.md` (new).
- Not touched: `runtime/src/services/`, `runtime/src/a32jit/`, `obj.h`, `gfx/src/nvnapi/`, shared title data, `orchestrations/`.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| 1. Interpreter lda/stl seq_cst + alignment fault; skip removed; test-a32 green | tested | `make -C runtime GFX=napple test-a32` rc 0: `excltest: 47/47 passed`; difftest ARM 11587 / Thumb 11743 compared, mismatches 0; inline words 1032723, 0 forms failed; progtest ok; `ok: no AArch32 interpreter`. `difftest --word` per form (2000 samples): ARM `lda` 931 alignment exits + 752 compared, `ldah` 686 + 800, `ldab` 931 compared, `stl*` the same; Thumb lda / ldah / stl / stlh 2000 compared each; 0 mismatches. `--family sync` seeds 2 / 3 / 7, ARM and Thumb: 0 mismatches |
| 2. Viewports 1-15 use their own swizzle; t_vp_swizzle case with viewport 1 != viewport 0 | tested | `MTL_DEBUG_LAYER=1 gfx/build/tests/t_vp_swizzle`: `ok (0 skipped)`, ten cases. Index-1 vertex program, vp0 identity / vp1 x=-x: right half. No-index program: left half. vp1 x<->y: horizontal half. Index 0 with vp0 x=-x: right half. Geometry program writing index 1: right half, then a horizontal half (log: `stage path ran: geometry ... drawn`) |
| 2. Capture-only passes | decided | no swizzle in capture passes (hardware order: viewport transform after transform feedback); the capture key zeroes all 16 |
| 2. `make -C gfx test` | tested | ALL TESTS PASSED (the four known-open SKIPs `t_clear_ds`, `t_clear_tex` x2, `t_feedback`, as in r4); `t_obj_canary: ok`, `t_xlate: ok`, `t_provoking: ok` |
| 2. LAS corpus | tested | `NAPPLE_SHADER_CORPUS=/Users/neto/NappleWork/shader-corpus/las gfx/build/tests/t_xlate` -> `corpus las: 5767 programs, 5767 translated, 5767 compile with Metal` |
| 3. 0 orphans under docs/process/archive; support matrix regenerated | verified | `python3 tools/check_docs.py` -> `checked 241 Markdown files: OK`; `python3 -I tools/support/gen_support.py --check` -> `support tables: OK` |
| 4. Four-title boots with `MTL_DEBUG_LAYER=1`: no regression | verified | head runs, r4 settings (60 s, ACNH 90 s, A at 25/40/55/70 s), run lock, cold private save roots; logs `/Users/neto/NappleWork/logs/leftovers-r5-head{,2}-<titleid>/`. All exit 0, 0 skipped draws, 0 warnings, no validation error. Warm run frames ACNH 2681 / LAS 3576 / SMO 3107 / MK8D 3565 vs r4 head 2682 / 3529 / 3227 / 3565 (r4 SMO base 2997). Screenshots vs r4 head: 0.00-0.07 except ACNH idle dialog 3.3 / 4.1 and LAS storm intro f-1000 13.3 (same scenes, checked side by side) |

## Unbuilt / untested list (the integrator checks these first)
- Nothing unbuilt.
- Not exercised on a title: no title sets viewport swizzles (no `viewport swizzles in use` line in the boots).
- The interpreter path runs only in the debug interpreter build and the tests; the shipped runtime has none.

## Findings
- **Multi-viewport rasterization is not implemented.**
  - The replay sets one Metal viewport (viewport 0's rectangle), and the viewport index a program writes reaches no `[[viewport_array_index]]`.
  - The index now selects the swizzle, but the primitive still lands in viewport 0's rectangle.
  - A follow-up would be `setViewports:count:` / `setScissorRects:count:` from `S.vp[16]` plus `[[viewport_array_index]]` in the vertex / pass-through output.
  - Metal allows that output from a vertex function on Apple GPUs.
  - `t_vp_swizzle` sets viewports 0 and 1 to the same rectangle for that reason.
- **Run-to-run noise in boots.** The first run of each title compiled every shader cold (11-24 ms on average, against 0.0-0.1 ms in r4's logs), with another
  session's runtime running at the same time. SMO came out at 2603 and MK8D at 3440. The warm rerun is within noise of r4.
  - Check against the warm numbers.
- **`gen_support.py --refresh-snapshot`** without an argument looks for `../.claude/skills/percent-analysis/history`, the old nested layout. In this
  flat checkout it fails ("no schema-2 snapshot"); pass the directory (`--refresh-snapshot .claude/skills/percent-analysis/history`). Not fixed
  (the generator is not mine); a one-line default change in `tools/support/gen_support.py` (`HISTORY`).
- **Stale comments outside my paths:**
  - `runtime/src/a32/a32_priv.h` says FL_ACQ is "acquire/release (ordering only)". Still true for the decode, but the exec is now seq_cst with an
    alignment check.
  - `docs/architecture/gfx.md` section 13 says "A program that selects another viewport gets viewport 0's swizzle"; see Needs.
- Pre-existing compiler warning in `gfx/tests/t_xlate_stages.inc:49` (`-Wformat`, flag ' ' with `%s`), not from this branch.

## How to integrate
- No order constraints.
- Conflict hotspots:
  - `gfx/src/mtl_shader.m` `TRANSLATOR_VERSION` (19): take the higher number and bump once more if both sides changed MSL.
  - `docs/index.md` "Process and history": this branch replaced r4's nested list of change notes with one line pointing at the changes README
    section. A branch that adds a note should add a row in `docs/archive/changes/README.md` ("Unattended rounds").
  - `docs/process/archive/README.md`: when the integrator moves r5's files into `docs/process/archive/`, add a "Round 2026-11-02-r5" table there,
    or regenerate it with the script in the change note's style. Otherwise `check_docs.py` reports them as orphans.
  - Support tables: regenerate after merge (`python3 -I tools/support/gen_support.py`), since the title notes may change.
- Build: `make -C gfx` then `make -C runtime GFX=napple`. `ShaderKey` grew by 28 bytes, so rebuild all gfx objects; the Makefile dependencies do.
  The disk shader cache is invalidated by the version bump.
- No Linux-only calls.

## How to test
1. `make -C runtime GFX=napple test-a32`: `excltest: 47/47 passed`, both difftest lines `mismatches 0`, `0 forms failed`, progtest ok,
   `ok: no AArch32 interpreter`. A `MISMATCH` on an `lda` / `stl` word means the interpreter and the lifted code disagree.
2. `make -C gfx test`: `ALL TESTS PASSED`, `t_vp_swizzle: ok (0 skipped)` with the lines `index 1, vp1 x = -x: ... row 0 ........########` and
   `GS index 1, vp1 x = -x: ... row 0 ........########`.
3. `NAPPLE_SHADER_CORPUS=/Users/neto/NappleWork/shader-corpus/las gfx/build/tests/t_xlate`: 5767 / 5767 / 5767.
4. `python3 tools/check_docs.py`: OK; `python3 -I tools/support/gen_support.py --check`: OK.
5. Four-title boots (`MTL_DEBUG_LAYER=1`, run lock): exit 0, 0 skipped draws, frames as above on a warm cache.

## Fix policy hints
- Test expectations, the docs tables and the generated support files are safe to adjust or regenerate directly.
- If a reviewer prefers the swizzle table in a buffer (fewer pipeline variants when titles change viewports 1-15 often), that is a design change:
  a `constant ushort[16]` argument at a fixed buffer index set per draw, and `vp_swizzle_vp` out of the key. No title calls SetViewportSwizzles,
  so the key form was chosen (no new binding, no per-draw upload).

## Needs from other areas
- `docs/architecture/gfx.md` section 13, "Viewport swizzles" (not in my owned paths). Replace "A program that selects another viewport gets viewport
  0's swizzle (logged once)" with: "Viewports 1-15 have their own (`ShaderKey.vp_swizzle_vp`): a program that writes the viewport index (vertex /
  tess eval attribute 0x68, or slot 0 `.z` of a geometry program's output) selects its viewport's swizzle from a 16-entry table at run time
  (`maxwell_vp_swizzle_msl`). Capture-only passes apply none. The viewport rectangle is still viewport 0's: multi-viewport rasterization is not
  implemented."
- `.claude/skills/percent-analysis/verified.json` `nvn/nvnCommandBufferSetViewportSwizzles`: update the note. Per-viewport swizzles are done,
  and the capture-only decision is above. It stays tested, not verified: no title calls it.
- `tools/support/gen_support.py`: default `HISTORY` for the flat layout (Findings).
- gfx owner: multi-viewport rasterization (Findings).
