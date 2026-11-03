# 2026-11-02 gfx-multi-viewport (batch 2026-11-02-r6, Mac)

Branch `claude/gfx-multi-viewport-2026-11-02-r6` (base `claude/leftovers-a32-viewport-docs-2026-11-02-r5` = `107ecc7`). Viewport arrays and
layered rendering on Metal: the 16 viewports / depth ranges / scissors reach the encoder, and the viewport index and layer a program writes route
its primitives.

## 1. Viewports, depth ranges, scissors (`gfx/src/mtl_replay.m`)
- `apply_raster_state` sets all 16 viewports and scissors on every draw (`setViewports:count:16`, `setScissorRects:count:16`). Each viewport gets
  the conversion viewport 0 always had: lower-left origin, render target scale, depth range from `S.dr[i]`. An unset or empty viewport covers the
  target, as viewport 0 did.
- The arrays are set again only when they changed within the encoder (`g_vp_serial`; the single-viewport clear / draw-texture passes reset it).
- `S.sc_set` is a bit per scissor (`OP_SCISSORS` sets the bits it writes). An unset scissor covers the target (`scissor_rect_of(i)`).
  - Before, the first `SetScissors` call made all 16 count, so viewports 1-15 would have had empty scissors.
  - Viewport 0 behaves as before. Clears and the other single-viewport passes still use `scissor_rect()` (= scissor 0).
- Every scissor is clamped to Metal's render pass size (`P.pw` / `P.ph`, recorded by `pass_begin`: the smallest attachment at its level).
  - Found in the first ACNH boot: a pass into mip level 1 of a 32 x 32 target is 16 x 16, and the unset scissors 1-15 covered the 32 x 32 guest
    size. `setScissorRects` aborted under the debug layer.
  - Scissor 0 only ever passed because the title's own scissor fitted.

## 2. Viewport index and layer outputs (`gfx/src/shader/`, `gfx/src/mtl_shader.m`, `gfx/src/mtl_stages.inc`)
- `ShaderInfo.writes_vp_index` / `writes_layer` (`maxwell.h`):
  - vertex / tess eval: a store to attribute 0x68 / 0x64;
  - geometry: a store to slot 0 `.z` / `.y` of the output record.
- Vertex / tess eval: `VOut` gains `uint vpi [[viewport_array_index]]` (the index clamped to 15) and, in a layered pass only,
  `uint layer [[render_target_array_index]]`. Both are set after the viewport swizzle / flip / depth conversions.
- Geometry: the pass-through vertex function (`gs_passthrough_fn`) gets the same two outputs from slot 0 `.z` / `.y`
  (`stage_pipe_key.gs_sys`, `GS_SYS_LAYER` / `GS_SYS_VP_INDEX`, part of its cache key).
- `ShaderKey.layered` / `pipe_key.layered`: Metal rejects a pipeline that writes `render_target_array_index` in a pass without
  `renderTargetArrayLength` (debug layer: "renderTargetArrayLength is set to zero"). The layer output therefore exists only in layered passes. In
  any other pass the program's layer is dropped, which is GL's behaviour for a single-image attachment.
- `TRANSLATOR_VERSION` 20.

## 3. Layered passes (`gfx/src/mtl_replay.m`)
- `pass_begin` computes `P.layers`: the array layers every attachment has from its slice. 2D array, 2D multisample array, cube (6) and cube array
  count; the view's layer count limits it; 2D and 3D textures count as 1.
- `pass_routing` runs at each draw that writes the viewport index or layer (plain draws in `prepare_draw`; geometry, tessellation and software
  tessellation draws through `stage_route`).
  - When the program writes the layer and the open pass is not layered but could be (`P.layers` >= 2), it ends the pass and reopens it with
    `renderTargetArrayLength = P.layers`. The draw then looks its pipeline up again with the layered key.
  - The layer counts from the attachment's slice.
  - Later draws in the same pass need nothing more: a program that writes no layer draws into layer 0 = the slice, as before.
- Clears folded into a pass's load action happen in the non-layered pass, so they still cover only the attachment's own slice (unchanged).
  NVN may clear every layer of a layered target; that is not checked here (see the integrator file).
- Logged once each: `viewport index written (program ...)`, `layer written (program ...): layered pass of N layers` and
  `layer written (program ...) with single-image render targets`.

## 4. Tests
- New `gfx/tests/t_multi_viewport.c`. One draw of three full-screen triangles, each with its viewport index and layer from an RGBA32UI attribute, into
  32x32 R32F; the fragment program writes its window depth, so each pixel tells the depth range used:
  1. 2D target: viewport 0's square (depth 0.5), viewport 1's square cut by scissor 1 to 8 x 16 (0.3), viewport 2's square in the other rows (0.8);
  2. a program without an index: viewport 0 only;
  3. a 2-layer array with (viewport, layer) = (0, 0) (1, 1) (2, 1): layer 0 holds viewport 0's square, layer 1 the other two;
  4. the same into a one-layer view of layer 1: all three squares land in layer 1;
  5. a geometry program that forwards the triangle with viewport index 2 and layer 1: viewport 2's square in layer 1 only.
  6. a quad patch whose tess eval program (Metal tessellation) writes viewport index 1 and layer 1: the 8 x 8 quarter of viewport 1 inside scissor 1,
     in layer 1 only. The software tessellator path (`SPK_TESS_SOFT`) runs the same epilogue and is not tested separately.
  7. into mip level 1 of a 32 x 32 texture (a 16 x 16 pass): viewport 0 covers the level. Without the clamp this case aborts the same way ACNH did.
- `gfx/tests/t_xlate.m` corpus mode: translates with `ShaderKey.layered = 1` and prints `corpus <tag>: N write the viewport index, M the layer`
  (the first files named). `NAPPLE_SHADER_CORPUS_XLATE_ONLY=1` translates without the Metal compiles (a quick scan).
- `gfx/tests/t_vp_swizzle.c`: the header comment no longer says only viewport 0's rectangle reaches Metal.
- Corpus scan (`t_xlate`): no program of the four titles writes the viewport index or the layer.
  - LAS: 5767 programs, translated and compiled.
  - ACNH: 8577, translated and compiled.
  - MK8D: 26544, translation only.
  - SMO: 31750, translation only.
- Four-title boots: no title reaches the new paths (no `viewport index written` / `layer written` line), so the routing is tested, not verified in
  a title. The viewport / scissor arrays themselves are set on every draw of every title.
- Full results: see the integrator file `integrators/claude-gfx-multi-viewport-2026-11-02-r6.md`.

## Files
`gfx/src/mtl_replay.m`, `gfx/src/mtl_stages.inc`, `gfx/src/mtl_shader.m`, `gfx/src/mtl_priv.h` (appended: `pipe_key.layered` + pad,
`stage_pipe_key.gs_sys`, `GS_SYS_*`), `gfx/src/shader/maxwell.h`, `gfx/src/shader/maxwell_translate.c`, `gfx/tests/t_multi_viewport.c` (new),
`gfx/tests/t_xlate.m`, `gfx/tests/t_vp_swizzle.c` (comment), `docs/architecture/gfx.md`.
