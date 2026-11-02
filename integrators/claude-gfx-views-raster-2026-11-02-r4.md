---
branch: claude/gfx-views-raster-2026-11-02-r4
orchestration: gfx-views-raster
base: ed33737
head: 460edec  # last work commit; the integrator-file commit sits on top
env: mac
date: 2026-11-02
areas: [gfx/shader, gfx/replay, gfx/api, docs]
status: ready-with-caveats
---

# claude/gfx-views-raster-2026-11-02-r4: stencil cube views, remaining integer views, separate-texture slot masks, viewport swizzles, provoking vertex

## Summary
All five acceptance items are done, including the two (4, 5) the orchestration allowed to be handed off.
- **1. Stencil sampling of cube / cube array depth-stencil views.**
  - Before: the translator refused it (the `fail()` at the old `maxwell_translate.c:3289`).
  - Now: declared `texturecube<uint>` / `texturecube_array<uint>` and read through `StencilTexCube` / `StencilTexCubeArray`. The direction selects the face and texel.
  - 3D stencil textures can't exist: Metal has no 3D depth formats. They stay refused.
- **2. Integer 1D / 1D array / cube array / 2D MS array textures.** Declared as integer types and read through `IntTex1D` / `IntTex1DArray` / `IntTexCubeArray` / `IntTex2DMSArray`. Only texel buffers and depth comparison keep the float declaration.
  - Two fixes found on the way:
    - the wrappers' lod queries returned `float2` / `float3`, so a TMML would have built `float2(float2, float2)`; they now return `float`;
    - buffer <-> texture copies of a 1D array now treat the region's y as the layer (Metal asserted).
- **3. Separate textures get the depth / stencil / integer slot masks** (the r3 finding).
  - New `ShaderKey.sep_*_slots` and `pipe_key.sep_*[2]`, filled from the separate texture bindings (`sep_slot_masks`).
  - The translator applies them to the texture word of a separate pair.
- **4. SetViewportSwizzles applied.**
  - Recorded as `OP_VP_SWIZZLE`. Viewport 0's swizzle (`ShaderKey.vp_swizzle`) is applied to the clip position of the stage that feeds the rasterizer: vertex, tess eval, and the geometry pass-through.
- **5. SetProvokingVertexMode first / last.**
  - Recorded as `OP_PROVOKING`. Mode 1 (last) re-emits direct draws as lists with each primitive's last vertex first.
  - Mode 0 is Metal's native first-vertex behaviour, unchanged.
- **Ownership extension:** `mtl_priv.h` (`pipe_key` fields), `mtl_stages.inc` (one line in `stage_key_masks`) and `obj.h` (two opcodes before `OP_COUNT`, enum only). Asked for at the start and approved by the orchestrator in 4209689. No `obj.h` struct changed; `t_obj_canary` is green.

## Files
`git diff --stat ed33737..HEAD` (gfx):
- `gfx/src/shader/maxwell_translate.c`:
  - `cube_face` / `cube_texel`;
  - `StencilTexCube` / `StencilTexCubeArray`;
  - `IntTex1D` / `1DArray` / `CubeArray` / `2DMSArray` (and `IntTexCube` rewritten on the shared helper);
  - `wrapper_name`, `sep_masks`, the vertex epilogue swizzle;
  - widened type checks.
- `gfx/src/shader/maxwell.h`: `ShaderKey.sep_*_slots`, `vp_swizzle` / `vp_pad`, `VP_SWIZZLE_IDENTITY`, updated mask comments.
- `gfx/src/mtl_replay.m`:
  - `sep_slot_masks`, `vp_swizzle_key`, `fill_pipe_key`;
  - `S.vp_swz` / `S.provoking_last` and the `OP_VP_SWIZZLE` / `OP_PROVOKING` cases;
  - `pv_last_*` and the draw hooks;
  - 1D array layers in `copy_region_dims`.
- `gfx/src/mtl_shader.m`: the masks and swizzle copied into the stage `ShaderKey`s (`build_pipe`, `stage_vs_key`, `stage_fs`, tess keys); the swizzle in `gs_passthrough_fn`; capture-key normalisation; `TRANSLATOR_VERSION` 18.
- `gfx/src/api_cmd.c`: `SetViewportSwizzles` and `SetProvokingVertexMode` record ops (the global `gfx_cmd_provoking_vertex_mode` is kept for `t_nvn_ext`).
- **Ownership extensions approved by the orchestrator** (4209689; confirmed by message on 2026-11-02):
  - `gfx/src/mtl_priv.h` (+4: `pipe_key.sep_depth/stencil/uint/sint[2]`, `vp_swizzle`, `pad_vp`);
  - `gfx/src/mtl_stages.inc` (+1: `sep_slot_masks` in `stage_key_masks`);
  - `gfx/src/obj.h` (enum: `OP_VP_SWIZZLE`, `OP_PROVOKING` before `OP_COUNT`).
- Tests: `t_stencil_sample.c` (cube cases), `t_int_tex.c` (7-10), `t_sep_sampler.c` (3-4), `t_xlate.m` (pins), new `t_vp_swizzle.c`, new `t_provoking.c`.
- Docs: `docs/architecture/gfx.md` (one paragraph in section 13, before "Rare control/system ops"), `docs/archive/changes/2026-11-02-claude-gfx-views-raster-2026-11-02-r4.md`.
- Not touched: `gfx/src/nvnapi/`, `runtime/`, `orchestrations/`, any `obj.h` struct.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| 1. Stencil cube / cube array | tested | `MTL_DEBUG_LAYER=1 gfx/build/tests/t_stencil_sample`: cube D24S8 (faces cleared to 0x10 + f) in directions +X / -Y / +Z / -Z -> `0x10 0x13 0x14 0x15`; cube array (12 layer-faces) cube 1 -X -> `0x17`, cube 0 +Y -> `0x12`, cube 1 +Z -> `0x1a`; `ok (0 skipped)`. `t_xlate` pins `texturecube<uint> t0_u` + `StencilTexCube`, `texturecube_array<uint>` + `StencilTexCubeArray`, a TLD4 gather on a stencil cube, all compiled by Metal. |
| 2. Integer 1D / 1D array / cube array / MS array | tested | `t_int_tex`: `1D RGBA8UI: d e f 10`, `1D array RGBA8UI layer 2: 25 24 23 22` (per-layer uploads), `cube array RGBA8UI cube 1 face -Y: 2c 2b 2a 29` (layer-face 9), `4x RGBA8UI array layer 1 sample 3: 5 6 7 8`; debug layer, no validation error. `t_xlate`: uint 1D, int 1D array, uint cube array (TEX and TLD4), uint 2D MS array (TLD.MS.ARRAY), int cube TMML, each compiled by Metal. |
| 3. Separate-texture masks | tested | `t_sep_sampler`: RGBA8UI at separate binding 1 -> `1 2 3 4`; stencil view of D24S8 at separate binding 2 (sampler word first in the OR) -> `0x5a`. Negative check: with `sep_masks` disabled the same draw fails Metal validation (`RGBA8Uint ... incompatible with ... Float`). |
| 4. Viewport swizzles | tested | `t_vp_swizzle`: the left half rectangle -> identity: left half; NEGATIVE_X: right half; x <-> y: the bottom 8 rows; identity again: left half. |
| 5. Provoking vertex | tested | `t_provoking`: triangle list FIRST 1 / LAST 3; strip FIRST {4, 5} / LAST {6, 7}; indexed strip with restart FIRST {4, 5} / LAST {6}; line list FIRST {8, 10} / LAST {9, 11}. A Metal probe (scratch program) confirmed Metal's own provoking vertex is the first one for lists, strips and lines. |
| 6. `make -C gfx test` | tested | ALL TESTS PASSED (the four SKIPs are the long-standing `t_clear_ds` / `t_clear_tex` / `t_feedback` known-open ones). `t_obj_canary: ok`, `t_nvn_ext: ok`. |
| 6. LAS corpus | tested | `NAPPLE_SHADER_CORPUS=/Users/neto/NappleWork/shader-corpus/las gfx/build/tests/t_xlate` -> `corpus las: 5767 programs, 5767 translated, 5767 compile with Metal` |
| 6. Four-title boots | **verified: no regression** | Base ed33737 vs head, 60 s (ACNH 90 s), A at 25 / 40 / 55 / 70 s, `MTL_DEBUG_LAYER=1`, cold private save roots (`/Users/neto/NappleWork/saves-r4-{base,head}`), run lock. Logs: `/Users/neto/NappleWork/logs/gfxraster-reg-{base,head}-<titleid>/`. Script: `regress4.sh` in this session's scratchpad. |

Boot details (row 6):
- **Results:** all four head runs exit 0 with 0 skipped draws.
- **Warnings and validation:** zero warning lines in every base and head log, and no Metal validation errors (only the "Validation Enabled" banner).
- **Frames, base -> head:** ACNH 2680 -> 2682, LAS 3521 -> 3529, SMO 2997 -> 3227, MK8D 3564 -> 3565.
- **Screenshot mean differences:**
  - LAS f-1000 17.3: the animated intro at another moment, checked side by side (round 3 had 16 for the same frame).
  - ACNH vs the base rerun 3.3 / 3.1: the same Nook Inc. dialog, idle animation.
  - All others 0.00-0.01.
- **The first base ACNH run crashed** (SIGBUS in guest SDK code at about 3 s, `gfxraster-reg-base-01006F8002326000`, before any draw of interest). A rerun of the same base binary with a fresh save root (`gfxraster-reg-base2-01006F8002326000`) exits 0. It's a base-tree flake outside gfx, reported in Findings.
- **New paths in the titles:** none of the head logs has a line from the new paths (`viewport swizzles in use`, `provoking vertex LAST`, `separate texture / sampler handles`, `SetProvokingVertexMode`). The titles don't exercise them in these scenes.

## verified.json keys to flip
`.claude/skills/percent-analysis/verified.json` isn't mine, so these are proposals:
- `gpu_fixed/texture views / sparse`: `partial` -> `impl`.
  - Note: integer views of every sampled target are now declared (1D / 1D array / cube array / 2D MS array added, `t_int_tex` 7-10), and 1D array copies take y as the layer.
  - Unchanged: cube-of-2D faces 1-5 replicate face 0 (a choice); no title uses sparse or integer views.
- `gpu_fixed/depth/stencil formats`: `partial` -> `impl`.
  - Note: stencil sampling of cube / cube array views (`t_stencil_sample`).
  - 3D stencil views cannot exist (Metal has no 3D depth formats).
- `nvn/nvnCommandBufferSetViewportSwizzles`: `stub` -> `impl`.
  - Note: `OP_VP_SWIZZLE`, applied in vertex / tess eval / GS pass-through (`t_vp_swizzle`).
  - Only viewport 0's swizzle applies (to every viewport); no title calls it.
- `nvn/nvnCommandBufferSetProvokingVertexMode`: `partial` -> `impl`.
  - Note: `OP_PROVOKING`; mode 1 reorders direct draws (`t_provoking`).
  - Indirect and geometry / tess draws keep the first vertex. The 0 / 1 = first / last mapping is a hypothesis (Maxwell register order). No title sets mode 1.
- The six `nvn/nvnCommandBufferBindSeparate*` keys: stay `partial`.
  - The slot-mask gap is closed (`t_sep_sampler` 3-4), but the c2 table offsets are still a hypothesis.
  - Update the note: "separate textures get no depth/stencil/integer slot masks" is no longer true.

## Unbuilt / untested list
- Nothing unbuilt.
- No path here ran in a title. None of the four titles, in the scenes reached, samples a stencil cube or the new integer view types, binds separate handles, sets viewport swizzles, or sets provoking mode 1; see the boot log check above.

## Findings
- **Provoking vertex mapping.**
  - Metal's provoking vertex is the first vertex. Measured on the M5: triangle list -> v0, strip triangle 1 -> v1, line -> v0.
  - The old `SetProvokingVertexMode` comment assumed mode 0 matches Metal, and this branch keeps that reading. It means 0 = first, 1 = last, as in the Maxwell `PROVOKING_VERTEX` register.
  - If NVN's enum is the GL order instead (`LAST = 0`), the fix is a one-liner in the `OP_PROVOKING` case (`S.provoking_last = mode == 0`). But that would rotate every draw of every title, so it needs evidence first.
- **Rotation on lines.** It reverses line direction, so endpoint pixels can differ by Metal's line rasterization rules. Only for mode 1, which no title sets.
- **Viewport swizzle scope.**
  - The swizzle applies to every viewport; real per-viewport swizzles would need the viewport index in the vertex function (a table in a buffer).
  - With a geometry program it applies in the pass-through, after transform feedback; capture-only passes don't apply it.
- **Separate masks scope.** `sep_masks` checks both words of the pair against the separate texture table (k < 32) and the combined table. A texture word read from a user uniform buffer can't be classified when the key is built (still float).
- **Test pitfall (not a bug).** A test fragment program with only two instructions has a zero word inside its first bundle, and the translator's end scan stops there. `t_provoking` pads with a third instruction.
- **Base-tree flake (outside gfx, not fixed).** One base (ed33737) ACNH boot crashed with SIGBUS at about 3 s: `CRASH signal=10 fault=0x79573bb226 pc=sdk+0x383f78`, right after the save mounts and the first copy-backed view. The rerun was clean, and the head run never hit it. It may be an ordering race for core-ordering-kernel to look at.
- **Same-second mtime race.** Editing a source file within the second its object file was built left the object stale (make compares whole seconds). Touch the source if an edit seems to be ignored.

## How to integrate
- No ordering constraint.
- Conflict hotspots:
  - `gfx/src/shader/maxwell_translate.c` / `maxwell.h`: unlikely, no other gfx session this round.
  - `TRANSLATOR_VERSION` 18: take the higher number on conflict.
  - `obj.h`: the enum tail. Keep both sides' opcodes before `OP_COUNT`.
- Build:
  - `make -C gfx test` -> ALL TESTS PASSED;
  - `make -C gfx adapter && make -C runtime`.

## How to test
1. `make -C gfx test`. Expect `t_stencil_sample: ok`, `t_int_tex: ok`, `t_sep_sampler: ok`, `t_vp_swizzle: ok`, `t_provoking: ok`, `t_xlate: ok`, `t_obj_canary: ok`.
2. `NAPPLE_SHADER_CORPUS=/Users/neto/NappleWork/shader-corpus/las gfx/build/tests/t_xlate` -> 5767 / 5767 / 5767.
3. Four-title boots as in the table. In a title that uses these paths, look for these lines:
   - `viewport swizzles in use (first: viewport 0 ...)`
   - `provoking vertex LAST in use (first: primitive ...)`
   - `separate texture / sampler handles in use`

## Fix policy hints
- Safe: `TRANSLATOR_VERSION` collisions; opcode order in the enum tail (they are only used by name).
- Needs a decision:
  - the provoking-vertex enum mapping (see Findings);
  - per-viewport swizzles if a title shows "viewports 1-15 have other swizzles".

## Needs from other areas
- None.
