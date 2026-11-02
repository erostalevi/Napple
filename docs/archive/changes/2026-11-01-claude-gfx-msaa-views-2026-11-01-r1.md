# 2026-11-01 claude/gfx-msaa-views-2026-11-01-r1: MSAA views, copy-backed texture views, sample shading, depth resolve, sparse

Batch 2026-11-01-r1, orchestration `gfx-msaa-views`. Mac (Apple M5), local only.

## What changed

### Copy-backed texture views (`gfx/src/mtl_tex.m`, `mtl_priv.h`, `mtl_replay.m`)
`mtl_tex_view` used to hand Metal any format view the game asked for (an invalid one aborts under the validation layer) and fell back to the whole texture for
type changes Metal cannot express. A view's format is now classified (`view_fmt_class`):

| class | when | result |
|---|---|---|
| VF_VIEW | plain colour formats of the same texel size, sRGB pairs | Metal format view (as before) |
| VF_DEPTH | R32F / R32UI / R32I of 32-bit float depth, R16 unorm of Depth16 | the depth texture itself, sampled as depth: the shader moves texel results into registers bit for bit, so the float depth *is* the integer view's value |
| VF_ALIAS | same bytes per texel / 4x4 block, another class: depth as packed colour (RGBA8 ...), Depth16 as R16UI, BC <-> 8/16-byte float colour, colour as depth; 4x Depth32F as 4x RGBA8 | a copy (`GfxAlias`) |
| VF_NONE | another texel size, ASTC, 24-bit depth read as bits, integer formats the translator cannot declare | the texture's own format, reported once (no invalid Metal view) |

Type changes Metal cannot express are copies too: 2D / 2D-array views of slices of a 3D texture, a 3D view of a 2D array.

A copy is refreshed when the source's write generation (`GfxTex.wgen`, bumped by `mtl_tex_written`: pass close for every attachment, copy / downsample /
clear destinations, written images, CPU uploads) differs from the one the copy holds. Queued copies are encoded with no encoder open: `pass_begin` and
`compute_encoder` flush them before opening; a draw / dispatch / DrawTexture whose bind finds a stale copy closes its pass (clears folded into the load are
stored), lets the copy run and reopens it loading the attachments, then binds again (once per draw). Copies: blit through a staging buffer (bytes, with
`MTLBlitOptionDepthFromDepthStencil` for D32F_S8), texture-to-texture blits per slice (3D <-> array), a per-sample render pass (multisampled depth ->
multisampled colour). Single-channel uint copies (R8/R16/R32 UINT) are bound as stencil slots (`texture2d<uint>`), the only integer sampling the translator
declares; `ds_slot_masks` asks `mtl_tex_samples_as_depth` / `mtl_tex_samples_as_uint` so the pipeline key matches what is bound.

### Sample shading
Already keyed (p_bind_program.sample_shading -> pipe_key -> ShaderKey, `[[sample_id]]`), and the per-sample pipeline runs. New finding: Metal keeps
`[[position]]` and default interpolants at the pixel centre even per sample, so every sample computes the same value. The fix is in the translator
(`gfx/src/shader/maxwell_translate.c`, not owned here): `floor(pos) + get_sample_position(sample_id)` for the fragment position and `[[sample_perspective]]` /
`[[sample_no_perspective]]` varyings when `key->sample_shading`. The patch (10 lines, tested here then reverted) landed on
`claude/gfx-stages-live-2026-11-01-r1` as 8e26ad2 (TRANSLATOR_VERSION 15); with both branches merged `t_sample_shading` passes its KNOWN checks. Any rate > 0
shades every sample (Metal has no minimum fraction).

### ResolveDepthBuffer
Kept as closing the pass with the depth stored, now documented as the full resolve on Metal: the console expands compressed depth (plane equations valid for
the sample locations they were made with) into explicit per-sample values; a Metal depth attachment always holds explicit per-sample values at the locations
they were rasterized with. The pass close also refreshes copy-backed views of the depth.

### Sparse textures
Documented fallback kept (fully resident; virtual pools are host memory, mapped tiles alias physical pages, unmapped tiles read zero). Metal sparse heaps were
not used: textures are filled from guest memory by CPU upload, which a private sparse heap cannot take without staging, and no title of the test set creates a
sparse texture. `t_sparse` checks the fallback. Not tracked: residency changes after a texture was uploaded (MapVirtual is a remap, not a write the dirty
tracker sees).

### Fixes found on the way
- `CopyTextureToBuffer` of a texture the CPU filled read the empty Metal texture: it now uploads the guest data first (`do_copy_xfer`).
- `mtl_tex_view` took a format view's component map from the texture's format instead of the view's.

### Diagnostics
Once-only info logs: multisampled textures in use, sample-shaded draws, ResolveDepthBuffer, Downsample, copy-backed views in use (+ refresh counts at powers
of two).

## Tests (gfx/tests)
- `t_view_copy.m`: every view class with readback (bytes, D32F_S8 depth plane, D24S8 none, BC1 <-> 8-byte colour, 4x depth -> 4x RGBA8, 3D <-> array,
  refresh / no refresh, size mismatch fallback).
- `t_view_copy_draw.c`: through the replay: R32UI of depth, R16UI copy of Depth16 refreshed mid-command-buffer (pass split), TLD.MS of 4x depth through R32UI,
  RGBA8 copy of depth sampled as unorm.
- `t_sample_shading.c`: per-sample pipeline; per-sample positions are a KNOWN gap until the translator patch lands.
- `t_sparse.c`: sparse texture on a virtual pool, mapped / unmapped tiles, GPU copy into a newly mapped tile.

## Status
See `integrators/claude-gfx-msaa-views-2026-11-01-r1.md` for the verified / tested breakdown and the title runs.
