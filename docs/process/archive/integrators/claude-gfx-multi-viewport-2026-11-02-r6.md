---
branch: claude/gfx-multi-viewport-2026-11-02-r6
orchestration: gfx-multi-viewport
base: 107ecc7 (claude/leftovers-a32-viewport-docs-2026-11-02-r5)
head: 964216f (code, tests and docs; the commit on top of it adds this file)
env: mac
date: 2026-11-02
areas: [gfx]
status: integrated
---

# claude/gfx-multi-viewport-2026-11-02-r6: viewport arrays, viewport index / layer outputs, layered passes on Metal

## Summary
- **Viewport, depth-range and scissor arrays.**
  - Every draw sets all 16 viewports (with their depth ranges) and all 16 scissors on the encoder: `setViewports:count:16` and
    `setScissorRects:count:16` in `apply_raster_state`.
  - Each viewport is converted the way viewport 0 always was (lower-left origin, render target scale).
  - Unset viewports and unset scissors cover the target (`S.sc_set` is now a bit per scissor).
  - Every scissor is clamped to Metal's real render pass size.
  - The arrays are re-sent only when they changed within the encoder.
- **Viewport index and layer outputs.**
  - Vertex and tess eval programs that store 0x68 / 0x64 get `[[viewport_array_index]]` / `[[render_target_array_index]]`.
  - Geometry programs that store slot 0 `.z` / `.y` get the same outputs through the pass-through vertex function.
  - This covers both GS-emulated paths and both tessellation paths.
  - `ShaderInfo.writes_vp_index` / `writes_layer` record what a program stores.
- **Layered passes on demand.**
  - Metal rejects a pipeline that writes the layer in a pass without `renderTargetArrayLength`.
  - So the first layer-writing draw in a pass whose attachments all have 2 or more layers reopens the pass layered, and the pipeline is looked up
    again with the layered key (`ShaderKey.layered` / `pipe_key.layered`).
  - With a single-image target (2D, a one-layer view, 3D), the layer is dropped, as GL does.
- **Tests and scan.**
  - New `t_multi_viewport`: 7 cases, all pass under `MTL_DEBUG_LAYER=1`.
  - `t_xlate` corpus mode counts viewport-index and layer writers per title. No program in the four-title corpus writes either.
- **Not done:**
  - Fragment-side reads of the layer / viewport index (attributes 0x64 / 0x68 as fragment inputs) still read 0. That is unchanged and not in the
    assignment.
  - 3D-texture layered rendering (the layer selecting a depth slice) is not done: a 3D attachment counts as a single image.

## Files
`git diff --stat 107ecc7..HEAD` (11 files, +520 / -37):
- `gfx/src/mtl_replay.m`:
  - viewport / scissor arrays (`apply_raster_state`, `scissor_rect_of`, `scissor_rect_clamp`, `S.sc_set` bits, `g_vp_serial`);
  - `passinfo.layers` / `layered` / `pw` / `ph`, `att_layers`, `renderTargetArrayLength` in `pass_begin`;
  - `pass_routing`, the pipeline lookup again in `prepare_draw`, `fill_pipe_key` sets `layered`.
- `gfx/src/mtl_stages.inc`: `stage_route` (geometry, tessellation, software tessellation draws), `te_sys`, `stage_pipe_key.gs_sys` filled.
- `gfx/src/mtl_shader.m`:
  - `gs_passthrough_fn` emits `vpi` / `layer`, with `sys` in its cache key;
  - the stage keys copy `layered`, and the capture normalisation clears it;
  - `TRANSLATOR_VERSION` 20.
- `gfx/src/mtl_priv.h` (appended only):
  - `pipe_key.layered` + `pad_layered[3]` after `pad_vp2`;
  - `stage_pipe_key.gs_sys` after `pad2`;
  - `enum { GS_SYS_LAYER, GS_SYS_VP_INDEX }`.
- `gfx/src/shader/maxwell.h`: `ShaderKey.layered` (end of struct), `ShaderInfo.writes_layer` / `writes_vp_index`.
- `gfx/src/shader/maxwell_translate.c`: the flags, and the `VOut` fields and assignments.
- `gfx/tests/t_multi_viewport.c` (new).
- `gfx/tests/t_xlate.m`: corpus counts, `ck.layered = 1`, `NAPPLE_SHADER_CORPUS_XLATE_ONLY`.
- `gfx/tests/t_vp_swizzle.c`: comment only.
- `docs/architecture/gfx.md`: a new paragraph after "Views and raster state", plus two stale sentences fixed:
  - the geometry section's "are the replay's to honour";
  - the viewport swizzle text: r5's "Needs" wording, now applied.
- `docs/archive/changes/2026-11-02-gfx-multi-viewport-2026-11-02-r6.md` (new).
- Not touched: `gfx/src/obj.h`, `gfx/src/nvnapi/`, `runtime/`, `crates/`, `orchestrations/`, `api_state.c`. The viewport / scissor / depth-range
  setters already record and replay all 16 indices (`OP_VIEWPORTS` / `OP_SCISSORS` / `OP_DEPTH_RANGES` with `first` + `n`).

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| 1. All 16 viewports / depth ranges / scissors on the encoder | tested + verified (set in titles) | `t_multi_viewport` cases 1-2 (below). The four title boots run every draw through `setViewports:count:16` / `setScissorRects:count:16` under the debug layer with no validation error |
| 1. ViewportIndex / Layer route primitives: vertex | tested | `MTL_DEBUG_LAYER=1 gfx/build/tests/t_multi_viewport`: `ok (0 skipped)`. Case 1: one draw, three triangles with viewport index 0 / 1 / 2 into a 32x32 R32F target. Viewport 0's 16x16 square has depth 0.5; viewport 1's has depth 0.3 and is cut to 8x16 by scissor 1; viewport 2's square is in the other rows, depth 0.8 (the pixel value is the window depth, so each depth range is checked). Case 2: a program without an index draws viewport 0 only. Case 3: a 2-layer array with (viewport, layer) = (0,0) (1,1) (2,1); layer 0 holds viewport 0 only, layer 1 holds viewports 1 and 2 (log `layer written ...: layered pass of 2 layers`). Case 4: a one-layer view of layer 1; all three land in layer 1 and layer 0 is untouched |
| 1. Geometry (GS-emulated pass-through) | tested | case 5: a geometry program writing viewport 2 / layer 1 draws viewport 2's square in layer 1 only (`stage path ran: geometry ... drawn`) |
| 1. Tess eval (Metal tessellation) | tested | case 6: a quad patch, tess eval writing viewport 1 / layer 1, draws an 8x8 quarter inside scissor 1, in layer 1 only (`stage path ran: tessellation`) |
| 1. Tess eval (software tessellator, `SPK_TESS_SOFT`) | untested | the same translator epilogue and the same `stage_route`; no case draws it |
| 1. Pass smaller than the guest size (mip level) | tested | case 7: draws into mip level 1 (16x16) and covers the level. Without the scissor clamp it aborts with `setScissorRects ... must be <= render pass width(16)`, the abort ACNH hit in the first boot (checked by disabling the clamp) |
| 2. `t_multi_viewport` under `MTL_DEBUG_LAYER=1` | tested | 7 cases, `t_multi_viewport: ok (0 skipped)` |
| 3. `make -C gfx test` | tested | `ALL TESTS PASSED`, final tree. The 4 known-open SKIPs are unchanged from r4 / r5: `t_clear_ds` 1, `t_clear_tex` 2, `t_feedback` 1. `t_obj_canary`, `t_vp_swizzle`, `t_provoking`, `t_stages`, `t_xlate`: ok |
| 3. LAS corpus | tested | `NAPPLE_SHADER_CORPUS=/Users/neto/NappleWork/shader-corpus/las gfx/build/tests/t_xlate`: `corpus las: 5767 programs, 5767 translated, 5767 compile with Metal` (with `ShaderKey.layered = 1`) |
| 3. ACNH corpus (full) | tested | `corpus acnh: 8577 programs, 8577 translated, 8577 compile with Metal` (877 s) |
| 3. MK8D / SMO corpus | tested (translation only) | `NAPPLE_SHADER_CORPUS_XLATE_ONLY=1`: mk8d 26544 / 26544 translated, smo 31750 / 31750 translated. The full Metal compile run was stopped after ACNH: at that rate it was more than an hour more and was slowing the boots |
| 3. Corpus scan: which titles write ViewportIndex / Layer | tested | `corpus <tag>: 0 write the viewport index, 0 the layer` for las, acnh, mk8d and smo: none of the 72,638 programs |
| 3. Four-title boots, debug layer, no regression | verified | final runtime (head `fe925d4` code), r4 / r5 settings (60 s, ACNH 90 s, A at 25 / 40 / 55 / 70 s), run lock, private save roots. Logs `/Users/neto/NappleWork/logs/gfxmv-r6-head2-<titleid>/`. All four exit 0, 0 skipped draws, 0 warnings, no validation error. Frames vs r5's warm runs (`leftovers-r5-head2-*`): ACNH 2679 / 2681, LAS 3336 / 3576 (reruns `gfxmv-r6-head3` / `head4-01006BB00C6F0000`: 3517, 3528; r4 head 3529), SMO 3265 / 3107, MK8D 3553 / 3565. Screenshots vs r5: 0.00-0.05 except ACNH idle 0.99 / 0.80 and the LAS storm intro f-1000 30.9 (same scene, later in the animation; checked side by side) |
| 3. Titles reach the new routing | not reached | no `viewport index written` / `layer written` line in any boot (consistent with the corpus scan). The routing is tested by `t_multi_viewport`, not verified in a title |

## Unbuilt / untested list (the integrator checks these first)
- Nothing unbuilt.
- Untested: the software tessellator draw with a layer / viewport-index-writing tess eval program (`SPK_TESS_SOFT` through `stage_route`).
- Untested: layered passes with a depth attachment (all cases are colour-only). `att_layers` takes the minimum over colour and depth attachments.
- No title writes the viewport index or layer, so the routing runs in tests only.

## Findings
- **Metal requires `renderTargetArrayLength` > 0 for a pipeline whose vertex function writes `[[render_target_array_index]]`.** The debug layer
  aborts: "the renderPipelineState has a vertex shader that writes to render_target_array_index but renderTargetArrayLength is set to zero". This is
  why the layer output is keyed (`ShaderKey.layered`) and the pass reopens layered on demand rather than every array pass being layered.
- **Decision: layered only on demand.**
  - Making every pass with array attachments layered would also change folded clears: a load-action clear then covers every layer, not just the
    attachment's slice.
  - Whether NVN's ClearColor / ClearDepthStencil clear every layer of a layered target is not known here. The current behaviour (the slice only) is
    kept, and the clear happens before the layered reopen.
  - If a title is found to rely on all-layer clears, that is a separate change in `do_clear_color` / `do_clear_ds`.
- **Scissor validation with a pass smaller than the guest size.**
  - The first ACNH boot aborted in `setScissorRects`. A pass into mip level 1 of a 32x32 target is 16x16, and the new "unset scissor = whole target"
    rectangles used the 32x32 guest size.
  - Scissor 0 had only ever passed because the title's own scissor fitted.
  - All scissors are now clamped to the real pass size, recorded in `pass_begin`. `t_multi_viewport` case 7 pins it.
- **Copy-backed views split a draw's pass** (`mtl_tex_alias_pending` in `prepare_draw`). The reopened pass now keeps `layered`, since the pipeline
  may write the layer.
- Run-to-run noise: LAS frames went 3336, then 3517 and 3528 in the reruns. Another session's runtime and the corpus scan's Metal compiles share the machine.
  The first head runs, made during the corpus scan, were lower: SMO 2457, MK8D 3185.
- `t_triangle` run on its own reports `screenshot file missing`. It needs the `make test` environment; it passes there. This is pre-existing and not
  from this branch.

## How to integrate
- Merge after `claude/leftovers-a32-viewport-docs-2026-11-02-r5`, which is the base: it has the per-viewport swizzles this branch builds on.
- Conflict hotspots:
  - `TRANSLATOR_VERSION`: 20 here. Take the higher value, and bump once more if both sides changed the MSL.
  - The end of `pipe_key` / `stage_pipe_key` in `mtl_priv.h` (appended fields).
  - `ShaderKey` / `ShaderInfo` ends in `maxwell.h`.
  - `docs/architecture/gfx.md`, after the "Views and raster state" paragraph.
- Build: `make -C gfx`, then `make -C gfx adapter && make -C runtime GFX=napple`.
  - The runtime Makefile does not relink when `libnapple_gfx.a` changes: remove `runtime/build/napple-runtime` first.
  - The `ShaderKey`, `ShaderInfo` and `pipe_key` sizes changed: rebuild all gfx objects (the Makefile dependencies do this).
  - The disk shader and pipeline caches are invalidated by the version bump and the `info_size` check.
- No Linux-only calls.

## How to test
1. `make -C gfx test`. Pass: `ALL TESTS PASSED` with `t_multi_viewport: ok (0 skipped)` and lines such as
   `2D, indices 0 1 2, layer 0: 640 non-zero; 0.5: 256 px (0,0)-(15,15); 0.3: 128 px (16,0)-(23,15); 0.8: 256 px (0,16)-(15,31)`.
   Fail: a `FAIL tests/t_multi_viewport.c:<line>` naming the case, or a Metal validation abort (scissor or render-target-array-index).
2. `NAPPLE_SHADER_CORPUS=/Users/neto/NappleWork/shader-corpus/las gfx/build/tests/t_xlate`. Pass: 5767 / 5767 / 5767 and
   `corpus las: 0 write the viewport index, 0 the layer`.
   For a quick scan of another title: `NAPPLE_SHADER_CORPUS_XLATE_ONLY=1 NAPPLE_SHADER_CORPUS=<dir> gfx/build/tests/t_xlate`.
3. Four-title boots with `MTL_DEBUG_LAYER=1` (run lock, the r4 / r5 settings). Pass: exit 0, `skipped draws 0`, no `failed assertion`, frames
   within noise of the numbers above. A `setScissorRects` / `setViewports` assertion means a pass size or clamp problem.
4. Nothing needs a human: no title exercises the routing.

## Fix policy hints
- Test expectations and doc wording are safe to adjust.
- Design choices, if a reviewer disagrees:
  - **Clears on layered targets.** Clearing all layers would mean making passes layered when the target is layered, or looping the clear over the
    layers.
  - **3D layered rendering.** `att_layers` would count `depth >> level` for 3D textures, with the layer becoming the depth plane.
  - **Viewport arrays only for index-writing pipelines.** Instead of all 16 always, set `count` 1 when the pipeline writes no index. This was not done:
    the redundant-set skip makes the arrays cheap, and the boots show no cost.

## Needs from other areas
- `docs/archive/changes/README.md` (not in my owned paths): add a section so `python3 tools/check_docs.py` stops reporting the new change note
  as an orphan (it reports exactly that one problem on this branch):
  ```
  ### Round 2026-11-02-r6
  | Note | Summary |
  |---|---|
  | [2026-11-02-gfx-multi-viewport-2026-11-02-r6](2026-11-02-gfx-multi-viewport-2026-11-02-r6.md) | Viewport / scissor / depth-range arrays on the encoder, viewport index and layer outputs, layered passes on demand. |
  ```
- `.claude/skills/percent-analysis/verified.json`:
  - viewport arrays (`nvnCommandBufferSetViewports` / `SetScissors` / `SetDepthRanges` with first > 0) and layered rendering: tested;
  - the routing is not verified in a title, since no corpus title writes the viewport index or the layer.

## Integration result
- Date 2026-11-02, merged into main as f57e56f (branch head 62c42e6), third of batch r6. Its base (`claude/leftovers-a32-viewport-docs-2026-11-02-r5`) was already on main. Outcome: **Clean**: one mechanical doc conflict in `docs/architecture/gfx.md` (main's r5 integration had reworded the same viewport-swizzle sentence: the branch's text kept, which drops the now-false "multi-viewport rasterization is not implemented", with main's "since 2026-11-02-r5"), plus one doc commit `integrate: gfx-multi-viewport row in the change archive` (3ee1dce, its Needs; check_docs reported the note as an orphan). No native ports (Metal throughout); the Maxwell translator is left portable on purpose. `TRANSLATOR_VERSION` 20 (main had 19).
- `make -C gfx`, `make -C gfx adapter`, relinked `make -C runtime GFX=napple` (binary removed first): rc 0, no new warnings (the test build's `nvn_test.h` unused-static warnings are the shared header's, pre-existing).
- `make -C gfx test`: **ALL TESTS PASSED**, `t_multi_viewport: ok (0 skipped)` under Metal API validation; the 4 known-open SKIPs unchanged (`t_clear_ds` 1, `t_clear_tex` 2, `t_feedback` 1).
- LAS corpus `NAPPLE_SHADER_CORPUS=/Users/neto/NappleWork/shader-corpus/las gfx/build/tests/t_xlate`: `5767 programs, 5767 translated, 5767 compile with Metal`; `0 write the viewport index, 0 the layer` (5 min).
- Four-title boots on the main tree (net-host-provider + error-applet-output + this), `MTL_DEBUG_LAYER=1`, `NAPPLE_INPUT_SCRIPT=25:A,40:A,55:A,70:A`, 60 s (ACNH 90 s), run lock, private save copies: SMO, LAS, MK8D, ACNH rc 0, `missing_ipc []`, `unsupported_svcs []`, stubbed only `nvdrv:13`, 0 skipped draws, 0 unimplemented calls, no failed assertion / validation error; frames SMO 2868, LAS 3573, MK8D 3564, ACNH 2678 (warm shader cache; other sessions shared the machine).
- Still untested (as declared): software-tessellator draws with a viewport-index / layer writer, layered passes with a depth attachment; no title writes the viewport index or layer, so the routing is tested, not verified in a title.
