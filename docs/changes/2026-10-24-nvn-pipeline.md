# NVN pipeline round: pipeline-build hang, NVN entry points, pipeline features (branch nvn-pipeline-2026-10-24)

Scope: `gfx/` except `gfx/src/shader/` (translator, owned by another session; nothing there was edited).
Tests: `make -C gfx test` passes (all old tests, plus `t_pipe_stress` and `t_nvnapi`). Game runs: see "Headless game runs".

## 1. Pipeline-build hang: reproduced, fix confirmed, second bug found
- **Reproduced.** New `gfx/tests/t_pipe_stress.c` draws 48 fragment programs x 4 blend modes (192 pipelines) into a fresh cache. A second process
  starts from that `pipelines.bin`, registers the programs (every pipeline is queued for prewarm at once), floods GCD's global queues with
  sleeping blocks and draws all 192 in reverse queue order from 1-16 threads, under a 120 s watchdog.
  The pre-fix `mtl_shader.m` (`9b86357^`, built from `git archive` in a scratch directory) **hangs 3 of 3 runs, and also with no flood and one
  thread**: 192 prewarm pipeline builds on the concurrent compile queue spin-wait for shader builds queued behind them, so they fill GCD's
  worker pool by themselves.
- **Fix confirmed** (current code: claim-before-build, serial `napple.gfx.pipe` queue, a waiter builds an unclaimed variant or pipeline itself).
  120 of 120 runs pass across 1 / 4 / 16 threads, archive on / off, shader prewarm on / off, flood on / off. Pipeline wait (`pipeline_wait_us`)
  is 3-10 ms per run for 192 pipelines; one outlier at 385 ms (1 thread, no archive, 512 blocked GCD blocks): Metal delivers pipeline compiler
  replies on GCD, so under that artificial starvation the inline build waits for GCD to add threads. Slow, not a hang.
- **Second bug, fixed (`gfx/src/mtl_core.m`).** The stress run lost draws about 1 run in 22 with 4 threads. Cause: the submission
  sequence number was taken outside the job-queue lock, so two threads submitting at once could queue seq N+1 ahead of N. Completion means
  "everything up to seq", so the replay finishing N+1 released every waiter on N (`nvnQueueFinish`, `nvnSyncWait`, deferred destruction
  through `gfx_retire`) before N ran. Any game submitting from two threads (or presenting from one while submitting from another) could read
  results early or free resources still in use. `jq_push` now takes the sequence under the lock. 0 lost draws in 200 runs after.
- `make test` runs the stress test (pass 1, then pass 2 with 1 and 4 threads, no archive).

## 2. NVN API surface
The %Analysis count of 104 missing came mostly from its scanner: `scan.py` matches `^NVN(` only, so the 92 `NVN_NOPROTO(` entries and the
macro-generated sampler getters count as missing (the skill lives outside `gfx/`, not changed here; matching `^NVN(_NOPROTO)?\(` and the
`SAMPLER_GETTERS` macro fixes it). The runtime's own count was 511 of 523 before this round and is **514 of 523** now.

Implemented (were undefined, empty, or returned constants):
| Entry points | Before | Now |
|---|---|---|
| `nvnCommandBufferDrawTexture` | undefined; `OP_DRAW_TEXTURE` ignored in replay | textured rectangle in the current pass (below) |
| `nvnCommandBufferMultiDrawArrays/ElementsIndirectCount` | undefined | GPU count (below) |
| `nvnBlendStateSetAdvanced{Mode,Overlap,PremultipliedSrc,NormalizedDst}` + 4 getters | empty / returned 0 | stored, read back, applied (below) |
| `nvnMultisampleStateSetSampleLocations`, `GetSampleLocationsGrid` | empty / 1x1 | stored, applied as Metal sample positions; grid = 16 / samples |
| `nvnTextureGetMemoryPool`, `GetMemoryOffset` | NULL / 0 | the pool and offset given to Initialize |
| `nvnTexture(Builder)GetRawStorageClass` | constant 1 | follows the storage class (one memory kind per class here) |
| `nvnCommandBufferGetMemoryCallbackData` | returned nothing (prototype said `void`) | returns the pointer set |
Prototype fixes in `nvnapi/nvn_protos.h` / `nvn.h`: `GetMemoryCallbackData` returns `void *`; `Texture(Builder)GetMemoryPool` and
`BufferBuilderGetMemoryPool` returned `NVNmemoryPool` by value; `NVNdrawTextureRegion` is `{xmin, ymin, xmax, ymax}` (was an opaque dummy).

Still undefined (9): transform feedback x7 and `nvnSyncInitializeFromFencedGLSync` / `nvnSyncCreateGLSync`. Their calls hit the unimplemented
thunk, which logs once and returns 0 (FALSE / no result). GL sync is OpenGL interop: there is no GL in the process, and the exact ABI is not in
the headers here, so FALSE is the honest answer.

Reviewed and left as they are, with the reason (no Metal counterpart, or nothing to do on this driver):
- debug labels / groups / markers, checkpoints (`nvnDeviceSet*Checkpoint*`, `InsertCheckpoint*`), external-debugger query, debug database walk;
- `Flush/InvalidateMappedRange`, `FlushTexels/InvalidateTexels` (unified memory; dirty tracking covers CPU writes);
- tiled cache actions and tile size, zcull save / restore / storage size, stencil cull criteria, colour reduction, subpixel precision bias,
  conservative raster, coverage modulation table, raster samples, alpha-to-coverage dither, sampler LOD snap and reduction filter, queue
  priority: performance hints or hardware features Apple GPUs do not expose;
- `nvnCommandBufferResolveDepthBuffer`: decompresses the bound depth target on the console; Metal depth targets are always readable;
- `SetCommandMemoryCallbackEnabled`, `GetControlMemoryUsed` = 0: recordings are host-side, command memory never runs out and control memory is
  not consumed, so the callback is never due;
- fast-clear colour registration (returns 0: no fast-clear slots).

Need translator work (not done; `ShaderKey` has no field for them): alpha test (`ColorStateSetAlphaTest`), logic ops (Apple GPUs have none;
needs framebuffer fetch), sample-rate shading (`ProgramSetSampleShading`), viewport swizzles, the non-Porter-Duff advanced blend modes
(framebuffer fetch), transform feedback capture, separate texture / sampler handles (below).

## 3. Pipeline features
- **DrawTexture** (`mtl_replay.m: do_draw_texture`): a screen-aligned rectangle (GL NV_draw_texture semantics: destination in window
  coordinates, source in normalized texture coordinates) drawn in the current pass with a pipeline matching the pass's attachments. The texel
  goes to every float colour target through the bound blend state and channel masks; integer targets are left alone; no depth / stencil test
  or write; the scissor applies. Both window origin modes (lower-left follows the `origin_flip` convention: rows stored bottom-up).
- **Multi-draw-indirect-count** (`do_draw_indirect_count`): Metal has no draw count from a buffer and the count is normally GPU-written
  (culling compute), so it cannot be read at encode time. A compute kernel copies the records of draws `i < min(*count, max)` (any stride)
  into scratch memory and zeroes the rest (a zero count draws nothing); then `max` indirect draws read the copy. It ends the open render pass
  (the next one loads). Verified with the count written by a GPU copy earlier in the same command buffer.
- **Advanced blend** (`mtl_shader.m: mtl_blend_resolve`): Porter-Duff modes, PLUS*, SCREEN and MINUS* map exactly to fixed-function factors for a
  premultiplied source and the uncorrelated overlap (ZERO / SRC / DST / PLUS / MINUS for any overlap; a non-premultiplied source where its
  factor is 0 or 1). Everything else is drawn as SRC_OVER and logged once per mode.
- **MSAA / depth resolve** (`do_downsample`): the source now keeps its samples (`StoreAndMultisampleResolve`; `MultisampleResolve` leaves them
  undefined; on Apple GPUs they happened to survive, so this is a spec fix that the test cannot tell apart). Resolves into the destination
  view's level / layer, from the source view's layer, resolves stencil too; integer formats and format / size mismatches are refused with a
  warning instead of failing Metal validation. Single-sampled sources are copied.
- **Programmable sample locations**: up to 8 locations (4-bit fixed point, Metal's 1/16 grid), set as the pass's sample positions; binding
  different ones mid-pass starts a new pass. Grid mode: Metal has one set of positions for all pixels, so the first pixel's locations apply.
- **Texture views on render targets**: a colour target bound through a view with another format (an sRGB view of an RGBA8 texture, a UINT view
  of a same-size float format) renders through a Metal format view, and the pipeline uses the view's format. Sampled views (levels, layers,
  format, swizzle) were already handled.
- **Separate texture / sampler binding: not done.** The replay side is trivial, but the shader side is not: a separate pair reaches the shader
  as two driver-constant loads combined with a LOP into a bindless TEX handle, and the translator refuses that ("bindless texture handle is
  not a constant buffer load"). It needs the translator to track the two loads and report both offsets in `ShaderTexture`; then the replay
  binds `S.septex` / `S.sepsamp`. The replay still warns once when a game binds them.
- **Transform feedback: not done.** Metal has no stream-out; capture means the vertex shader writing its outputs to a buffer, which is
  translator work (and the XFB layout comes from the program's control section, parsed in `shader/blob.c`). Geometry and tessellation shaders:
  not started (translator work, needs your go-ahead as agreed).

## Tests (all through the public NVN API, headless Metal, pixel readback)
- `t_pipe_stress`: above.
- `t_nvnapi`: DrawTexture in both origin modes (texel placement, nothing outside the rectangle); MultiDrawArraysIndirectCount with a GPU-written
  count and MultiDrawElementsIndirectCount; 10 advanced blend cases against hand-computed results; programmable sample locations moving an
  edge pixel in and out of coverage at 4x MSAA, resolved through Downsample, plus a second resolve after more drawing; an sRGB render target
  view (0.5 stored as 0xbc vs 0x80); the getters.
Only unit-tested (no game exercises them in the runs below, as far as the logs show): DrawTexture, indirect count, advanced blend, sample
locations, render target view formats, Downsample changes.

## Headless game runs
Runtime built from this branch (`make -C runtime GFX=napple`), `--headless --seconds 60`, save root and shader cache in a scratch directory
(the system fonts copied in; without them both games crash at boot on zero-size fonts, which has nothing to do with gfx). Each game cold
(empty cache), then warm (prewarm from the `pipelines.bin` the cold run wrote), under `work/gpu.lock`. No input was given.
| Run | Result | Pipelines | NVN |
|---|---|---|---|
| SMO cold / warm | 60 s, exit 0; mode-select screen at frame 2400 | 5 (warm: 5 of 5 prewarmed, 5 of 5 from the archive) | 514 of 523, 0 unimplemented calls |
| LAS cold / warm | 60 s, exit 0; intro scene at frame 2400 | 12 (warm: 11 prewarmed, 12 of 12 from the archive) | 514 of 523, 0 unimplemented calls |
This only shows that nothing regressed through boot and the title / intro. These runs never got in-game, so the prewarm load is tiny compared
with the reported hang (a large `pipelines.bin` after a long session); `t_pipe_stress` covers that case. Worth a windowed run into a
level, with a long-session cache, on your side.

## Files
- `gfx/src/mtl_core.m` (sequence under the job lock), `gfx/src/mtl_replay.m` (DrawTexture, indirect count, Downsample, sample positions,
  view formats), `gfx/src/mtl_shader.m` (`mtl_blend_resolve`, exported blend mappings), `gfx/src/mtl_tex.m` (attachment format views),
  `gfx/src/mtl_priv.h`, `gfx/src/obj.h` (payloads, blend / multisample / texture fields), `gfx/src/api_cmd.c`, `gfx/src/api_state.c`,
  `gfx/src/api_texture.c`, `gfx/src/nvnapi/nvn.h`, `gfx/src/nvnapi/nvn_protos.h`, `gfx/Makefile`, `gfx/tests/t_pipe_stress.c`, `gfx/tests/t_nvnapi.c`.
