---
branch: claude/gfx-msaa-views-2026-11-01-r1
orchestration: gfx-msaa-views
base: df2f318
head: ee967c2 (code at bb08672; the integrator-file commit follows)
env: mac
date: 2026-11-01
areas: [gfx]
status: integrated
---

# claude/gfx-msaa-views-2026-11-01-r1: MSAA views, copy-backed texture views, sample shading, depth resolve, sparse

## Summary
- **Copy-backed texture views** (`mtl_tex.m` GfxAlias): sampled views Metal cannot make over the storage are a copy kept current by a per-texture write generation.
  Covers: depth read as packed colour (RGBA8 ...), Depth16 as R16UI (bound as a uint/stencil slot), BC <-> 8/16-byte float colour, colour as depth, 4x Depth32F
  as 4x RGBA8 (per-sample pass), 2D / 2D-array views of slices of a 3D texture, 3D view of a 2D array. The replay encodes queued copies before a pass / compute
  encoder opens, and splits an open pass (once per draw) when a bind finds a copy stale.
- R32F / R32UI / R32I views of 32-bit float depth now sample the depth texture (same register bits) instead of asking Metal for a depth->colour view (invalid).
  Views of another texel size keep the texture format (previously an invalid Metal view).
- **Sample shading**: was keyed already; found that Metal keeps `[[position]]` and interpolants at the pixel centre per sample, so the per-sample pipeline gave
  identical samples. Translator fix written + tested here, **landed on `claude/gfx-stages-live-2026-11-01-r1` as 8e26ad2** (their area). `t_sample_shading`
  carries KNOWN checks that pass once both branches are merged.
- **ResolveDepthBuffer**: still "close the pass with depth stored", now documented as the complete resolve on Metal (explicit per-sample depth); pass close
  refreshes copy-backed views of the depth. Not changed in `api_cmd.c`.
- **Sparse**: documented fully-resident fallback kept and tested (`t_sparse`); Metal sparse heaps not used (CPU-uploaded textures; no test title creates one).
- Bug fix: `CopyTextureToBuffer` of a CPU-filled texture returned the empty Metal texture; it now uploads the guest data first.
- NOT done: no title in the test set uses MSAA, sample shading, Downsample or ResolveDepthBuffer (once-only logs added; none fired in 90 s runs of MK8D, SMO,
  LAS, ACNH), so acceptance 2 for MSAA / sample shading is unit-tested only. Integer sampling beyond single-channel uint (RG32UI etc.) needs the translator to
  declare integer textures (not done, VF_NONE fallback).

## Files
`git diff --stat df2f318..HEAD`:
- `gfx/src/mtl_tex.m` (view classification, GfxAlias, flush, diagnostics), `gfx/src/mtl_priv.h` (GfxTex.aliases / wgen, alias API), `gfx/src/mtl_replay.m`
  (write generation bumps, alias flush / pass split, ds slot masks, readback upload, MSAA logs, ResolveDepthBuffer doc), `gfx/src/api_program.c` (comment).
- New tests: `gfx/tests/t_view_copy.m`, `t_view_copy_draw.c`, `t_sample_shading.c`, `t_sparse.c`.
- `docs/archive/changes/2026-11-01-gfx-msaa-views-2026-11-01-r1.md`, this file.
- No shared files touched. `mtl_stages.inc`, `api_cmd.c`, `shader/`, `mtl_shader.m` untouched.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Acceptance 1: `make -C gfx test` with new tests | tested | `make -C gfx -j4 test` -> ALL TESTS PASSED (t_view_copy, t_view_copy_draw, t_sample_shading 1 KNOWN skip, t_sparse ok; MTL_DEBUG_LAYER=1) |
| Copy-backed views, every class with readback | tested | t_view_copy: depth->RGBA8 bytes, Depth16->R16UI codes, D32F_S8 plane, BC1<->RG32F/RG32UI->BC1 blocks, 4x depth->4x RGBA8 per sample, 3D<->array slices, refresh/no-refresh |
| Pass split mid-command-buffer | tested | t_view_copy_draw: R16UI copy refreshed after a clear in the same command buffer (0xffff), draws on both sides land |
| 3D texture sampled as 2D on a real title | verified | ACNH (01006F8002326000, 90 s): `copy-backed texture views in use (first: kind 3, format 92 -> 92, type 7 -> 2 ...)`, 1 refresh; base build logged `texture view not expressible ... using the whole texture` for it. Frame f-1500 identical to base (mean diff 1.25/255) |
| Sample shading per-sample positions | tested (with 8e26ad2 applied locally) / KNOWN on this branch alone | t_sample_shading: 3.375 3.875 3.125 3.625 with the patch, 3.5 x4 without |
| Acceptance 2: MSAA / sample shading on a real title | not possible with this set | once-only logs (`multisampled textures in use`, `sample shading:`, `Downsample`, `ResolveDepthBuffer`) never fired in MK8D / SMO / LAS / ACNH 90 s runs |
| Acceptance 3: no regression, LAS / SMO / MK8D / ACNH headless | verified | 90 s each under the run lock, base (df2f318) vs head: exit 0, frames 5332/5248/5237/2674 vs 5173/4714/5248/2625, 0 skipped draws, 0 unimplemented; screenshots checked by eye (differences are scene timing only). ACNH 75 s with MTL_DEBUG_LAYER=1 (`gfxmsaa-val-acnh`): copy path ran, no failed assertion, exit 0 |
| tools/golden | not available | no goldens on this Mac (`~/Library/Application Support/Napple/goldens` missing) |
| Sparse fallback | tested | t_sparse: mapped tile reads physical pages, unmapped zero, GPU copy into newly mapped tile |
| Readback upload fix | tested | t_sparse failed before the fix (zeros), passes after |

Logs: `/Users/neto/NappleWork/logs/gfxmsaa-{base,new}-{mk8d,smo,las,acnh}/` (run.log, shots/, the exact runtime binary).

## Unbuilt / untested list (the integrator checks these first)
- None unbuilt. Untested on a real title: AK_BYTES / AK_MS_DEPTH copy kinds, the draw/dispatch pass split, DrawTexture split (unit/replay tests only).

## Findings
- Metal `[[position]]` is the pixel centre even in a fragment function declaring `[[sample_id]]`; only `get_sample_position()` and `[[sample_perspective]]`
  inputs move per sample (checked with a standalone Metal probe on Apple M5). Fixed in the translator by gfx-stages-live (8e26ad2, from this session's patch).
- The translator declares every sampled texture as float except depth / stencil slots, so integer views (R32UI of RGBA8, RG32UI of BC, ...) cannot be bound as
  integer textures. R32* views of float depth are served by the depth path (identical bits); single-channel uint copies by the stencil-slot path; others fall back.
- Bug (fixed): T2B readback of CPU-uploaded textures skipped the upload.
- Not fixed (outside area): ACNH looks up `nvnTextureBuilderGetViewOffset`, MK8D `nvnDeviceWaitForError` (unknown names; need `gfx/src/nvnapi/names.txt`
  + an implementation; trivial: builder variant of nvnTextureGetViewOffset).
- Not fixed: sparse residency changes after a texture was uploaded are not seen (MapVirtual remaps pages; dirty tracking only sees writes). No title hit it.
- Decision: once-per-draw pass split instead of pre-scanning bindings (bind_stage resolves bindless handles; duplicating that was riskier). Copies of a pass's own
  attachment (e.g. soft particles) refresh once per pass, not per draw (alias gen is taken at flush time).

## How to integrate
- Independent of the other r1 branches except gfx-stages-live: both touch `gfx/` but no common files (they own `mtl_stages.inc`, `shader/`, `mtl_shader.m`,
  `api_cmd.c`). Expected clean merge.
- **After both this branch and `claude/gfx-stages-live-2026-11-01-r1` are merged**, add (requested by gfx-stages-live; their function lives in their
  `mtl_stages.inc`) at the top of `do_draw_indirect_count` in `gfx/src/mtl_replay.m`: `if (stage_draw_indirect_count(d)) return;`. Not added here: it does not
  build without their branch. Then `t_sample_shading` should report 0 skipped.
- Build: `make -C gfx -j4 test` (ALL TESTS PASSED), `make -C gfx lib adapter && make -C runtime` (needs `make -C input/audio/frontend` once in a fresh worktree).

## How to test
1. `make -C gfx -j4 test` -> ALL TESTS PASSED; `gfx/build/tests/t_view_copy`, `t_view_copy_draw`, `t_sparse` print `ok`; `t_sample_shading` prints `ok (1 skipped)`
   alone, `ok (0 skipped)` with gfx-stages-live merged (T_STRICT=1 makes the skip a failure).
2. ACNH headless 90 s (A presses at 25/40/55/70 s): log has `copy-backed texture views in use (first: kind 3, format 92 -> 92, type 7 -> 2` and no
   `texture view not expressible ... using the whole texture`.
3. Regression: LAS / SMO / MK8D / ACNH 60-90 s boots, 0 skipped draws, screenshots normal; with MTL_DEBUG_LAYER=1 no failed assertion.
4. Human: none required; ACNH title / check-in counter scene is where the 3D LUT copy is used.

## Fix policy hints
- A Metal validation abort in `alias_copy_blit` / `alias_copy_ms` / `alias_view`: safe to make that case return VF_NONE in `view_fmt_class` (falls back to the
  texture's own format, the old behaviour minus the invalid view).
- Performance problem from pass splits (look for `copy-backed texture views: N refreshes` growing fast): design decision; options are pre-scanning bindings
  before `pass_begin`, or tolerating one-pass-stale copies.

## Needs from other areas
- (done) Translator per-sample inputs: landed on gfx-stages-live as 8e26ad2.
- Translator: integer sampled textures (`texture2d<uint/int>` by format, like `image_int_mask`) so multi-channel integer views can be copies too (gfx-stages-live / shader owner).
- `gfx/src/nvnapi/names.txt` + api: `nvnTextureBuilderGetViewOffset`, `nvnDeviceWaitForError` lookups seen in ACNH / MK8D.

## Integration result
- Date: 2026-11-01. Merged into local `main` as 550afc8 (`--no-ff` merge of 7197269), after gfx-stages-live (which carries this branch's sample-shading translator patch as 8e26ad2). Outcome: **Small fix** (one `integrate:` commit, 284e5a7, asked for by both hand-offs): the one-line `if (stage_draw_indirect_count(d)) return;` at the top of `do_draw_indirect_count` in `gfx/src/mtl_replay.m`, `__attribute__((unused))` dropped from `stage_draw_indirect_count` (`mtl_stages.inc`) and the two `NAPPLE_TEST_IND_COUNT_STAGES` guards removed from `gfx/tests/t_stages.c` (test 2e now always built). No native ports (Metal-only code already).
- Tests on this Mac: `make -C gfx -j6 test` -> ALL TESTS PASSED both before and after the hook (t_view_copy ok, t_view_copy_draw ok, t_sparse ok, t_stages ok 0 skipped incl. 2e, t_msaa ok, t_resolve_depth ok); `T_STRICT=1 MTL_DEBUG_LAYER=1 gfx/build/tests/t_sample_shading` -> **ok (0 skipped)**, per sample 3.375 3.875 3.125 3.625 vs per pixel 3.5 x4 (the KNOWN checks pass with both branches merged). `t_stages` 2e logs `MultiDraw*IndirectCount with transform feedback / geometry / tessellation: count and arguments read at encode time (2 of 3 draws)`. Full rebuild of gfx/audio/input/frontend/runtime clean; `make -C audio|input|runtime test` and `runtime test-services` (60 tests, 0 failed) rc 0.
- Four-title regression boots on the combined tree (all four r1 branches + the hook; scratch runtime, run lock, audio device off, copied saves, `NAPPLE_SCREENSHOT`): LAS 60 s 3524 frames, SMO 60 s 3521 frames, MK8D 60 s 3493 frames, ACNH 90 s 2665 frames; all exit 0, 0 skipped draws, 0 unimplemented calls, no Metal validation line; warning counts LAS 0 / SMO 0 / MK8D 2 / ACNH 28 (the same sets the session runs reported). Screenshots checked by eye: LAS beach, SMO Bowser fireball scene, MK8D engine-class menu with the NVDEC preview panel, ACNH Nook Inc. counter, all rendered correctly.
- ACNH (How to test 2): `copy-backed texture views in use (first: kind 3, format 92 -> 92, type 7 -> 2, levels 0+1 slices 0+1)`, `1 refreshes`, and no `... using the whole texture` line. A different fallback note appeared once: `texture view type 5 of a type 2 texture is not expressible: viewed as type 2 over levels 0+4 slices 0+1` (a cube-type view of a 2D texture; not covered by the copy kinds, left as is).
- Still unverified: MSAA / sample shading / Downsample / ResolveDepthBuffer on a real title (none of the four uses them), AK_BYTES / AK_MS_DEPTH copy kinds and the pass split on a real title.
