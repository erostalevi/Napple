# 2026-10-27 gfx-depth-views (branch claude/gfx-depth-views-2026-10-27)

Changes (gfx only):
- `nvnCommandBufferResolveDepthBuffer` is real: recorded as an `OP_BARRIER` with the private bit `BARRIER_RESOLVE_DEPTH`; the replay flushes pending clears and ends the open pass so the depth of the pass is stored before a later sample or copy. MSAA depth resolve itself stays `Downsample` (StoreAndMultisampleResolve, depth and stencil).
- Depth-stencil sampled views: `mtl_tex_view` takes the texture's `ds_mode`; stencil mode (texture's or the view's, the view overrides) gives a stencil-only format view (`X32_Stencil8`, or `Stencil8`). D24S8 is stored as Depth32F_Stencil8, so it gets the same view. Depth mode is the texture itself.
- `nvnProgramSetSampleShading` stored in a side table keyed by the program address (enable, rate clamped 0..1; `gfx_program_sample_shading` test hook). NOT in the program object: a first version that grew `prog_t` past 152 bytes crashed ACNH at boot (the guest NVNprogram is smaller than the 0xC0 the header assumes); keep prog_t at 152 bytes. Metal has no pipeline switch for it (per-sample shading follows `[[sample_id]]` in the shader), so nothing enters the pipeline key; documented in code.
- `nvnProgramSetDebugLabel` restored (NVN 523/523 again in the scan).
- View fallback: a view whose type Metal cannot express (cube of one layer, ...) now keeps the level and layer range as the texture's own kind instead of the whole texture. Real sparse textures (MTLSparseTexture) are not done: documented as not cheap (needs heaps and placement); the existing residency calls stay no-ops.
- MK8D "colour attachments 3 and 4 are the same image" is now an info line (the duplicate is a legitimate drop) and the dropped slot's pipeline format is cleared, so the follow-up "attachment 4 is format 0 but pipeline said 90" warning cannot occur.
- Draws with a zero index count are skipped (Metal validation aborted an SMO run on one).
- SMO "shader storage buffer is not inside a memory pool" was a dangling slot of the 16-entry driver-constant table (NVN allows a stale binding after its pool is finalized): now an info line, the slot reads zero. (Originally listed below as not reproduced; it did reproduce in the full 260 s route.) Old text: "shader storage buffer is not inside a memory pool" and ACNH's 14 unknown entry points did not reproduce on this base (45 s SMO run, archived harness logs: 0 unimplemented entry points); no change made for them.

Tests (all with MTL_DEBUG_LAYER=1, in `make -C gfx test`): t_resolve_depth, t_ds_views, t_tex_views. Existing t_d24, t_msres_int, t_clear_ds unchanged and green.
Files: gfx/src/{api_cmd.c,api_program.c,mtl_replay.m,mtl_tex.m,mtl_priv.h,obj.h}, gfx/tests/t_{resolve_depth.c,ds_views.m,tex_views.m}.
Not verified: shader-side stencil sampling (the translator declares the sampled type; a uint stencil binding was not exercised by a shader).

Build note: in gfx/, plain `make` builds only the first dependency (the Makefile's `-include` of .d files changes the default goal): use `make lib` (or `make tests`).
ACNH cold boot is racy at the base (about 1 run in 10 dies in game code at the olsc:u open, also with the unmodified base gfx); not from this work.
