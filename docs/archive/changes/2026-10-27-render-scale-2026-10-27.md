# render-scale (2026-10-27): custom render scale, correct output on any screen, upscaling and supersampling

Branch `claude/render-scale-2026-10-27` (Mac, verified on an Apple M5 headless with Metal API validation on). Design and reference: [gfx.md section 12](../../architecture/gfx.md); screenshots: [screenshots/render-scale](../../screenshots/render-scale/README.md).

## What changed
- **Free render scale 10..800 %** (`internal_scale_percent`, was 25..400), live without relaunch (overlay text field "Custom scale", presets 0.5x/1x/2x/4x, new "4K" and "1440p" output targets = fixed size mode). Out of range values clamp; settings round trip in `frontend/tests/test_settings.c` (every 37th percent, upscaler 0..3, aspect 7).
- **Output fit** is now the pure function `napple_display_fit_rect` (letterbox/pillarbox, stretch, fill = crop, and the new *whole-number multiple* mode, `aspect` 7) used by `mtl_present_compose`; unit tested for 16:9 / 21:9 / 16:10 / 4:3 / 9:16 targets and proven identical to the previous inline code (`t_scale_math`, 1,000+ cases).
- **Own spatial upscaler** `gfx/src/mtl_upscale.m` (original code): structure-tensor driven anisotropic Lanczos-2 with an anti-ringing clamp + limited-lobe sharpening. `upscaler` enum: 0 none, 1 MetalFX spatial, 2 built-in (FSR-like), 3 CAS. MetalFX absent -> falls back to 2 and the overlay says so (`napple_display_effective_upscaler`). Every upscaler renders at window / `upscale_factor` (1.5); this was MetalFX-only before.
- **MetalFX temporal**: not offered. It needs depth and motion vectors from the title; none of the four provides them (decision, recorded in the UI note and gfx.md).
- **Budget**: `napple_display_budget_pixels` + `compute_scale` now give up supersampling first, then reduce the scale uniformly; 4x + 4x SSAA degrades to ~2.7x instead of failing. 60 s no-crash runs at 4x on all four titles (below).
- **Fixes found by running the titles** (they were invisible at 1x):
  1. `NVN_TEXTURE_FLAGS_IMAGE` vetoed the scaling of every screen-sized target of LAS, SMO and ACNH (they set it on all of them): render scale did nothing on LAS (all 46 distinct targets kept their size). The veto now applies only to CPU-filled image textures. `NAPPLE_SCALE_DIAG=1` logs each distinct target once with the reason.
  2. Render passes whose attachments disagree (a mipped attachment cannot be re-allocated) aborted on a scissor larger than the pass at factors below 1 (ACNH, MK8D, SMO at 0.375-0.45x in a portrait window) and silently rendered only the overlap above 1. `pass_unify_scale` now unifies factors below 1 and, when an attachment cannot be lifted, brings its scaled siblings back to 1x.
- **Per-title table** `gfx/src/mtl_scale_table.m` (JSON: `disable`, `max_scale`, `min_div`, `skip`/`force` size lists, `report_aspect`), built-in rules plus `NAPPLE_RSCALE_TABLE`. The audit (below) needed no behaviour rule; the four titles carry notes.
- Headless `NAPPLE_OUTPUT_SIZE=WxH` (drawable size, runtime/src/main.c) so screenshots can be taken at any window shape; `tools/golden/render_scale.py` (shots / psnr / stress).

## Audit of resolution dependent effects (LAS, SMO, ACNH, MK8D)
From `NAPPLE_SCALE_DIAG` logs at 4x and screenshots at 1x/2x/4x: all screen-shaped targets scale uniformly (LAS blur chain 960x540..240x135 and depth copies, SMO 1600x900 G-buffer and 400x225, ACNH 1920x1080 and 480x272, MK8D 1920x1080 and 1280x720). Blur/bloom taps are UV offsets from the guest's texel size, so their extent keeps its fraction of the screen; fragment positions go through the rscale table. SMO 1x vs 2x differs only at edges (error map), nothing regional. Shadow maps, cube/mipped/image-only textures and small utility targets keep their size. "Match window aspect" (aspect 6) shows no change in ACNH / SMO / MK8D at 21:9 (they draw 16:9 regardless), so no title needs `report_aspect: false`; the mechanism exists (HUD-safe fallback) for when one does.

## Verified (Mac, this session)
| Item | Evidence |
|---|---|
| Unit tests | `make -C gfx test`: `t_scale_math`, `t_scale_alloc`, `t_scale_table`, `t_scale_upscale`, `t_display`, `t_present` and the older ones pass with `MTL_DEBUG_LAYER=1`; `make -C frontend build/test_settings` 61 checks. |
| Upscaler matrix | `t_scale_upscale`: built-in sharpness 4.37 vs bilinear 2.74, CAS 4.07, Lanczos 3.82; PSNR on edges 36.1 dB (bilinear 35.3); sheet [upscaler-matrix.png](../../screenshots/render-scale/upscaler-matrix.png). |
| Screenshots | 38 captures: 4 titles x 1x/2x/4x/21:9/9:16, ACNH 0.5x/1.37x/SSAA2/4K/21:9 fill/16:10/4:3/9:16 native, fsr/cas/metalfx on ACNH and SMO, aspect report; every size equals the drawable. |
| 1x PSNR vs the pre-change binary | best matching frame pair (the guest animates in real time): SMO 99 dB (identical, 2 scenes), LAS 99, MK8D 99, ACNH 72 dB. |
| 4x 60 s runs | `render_scale.py stress all`: exit 0, no gfx warning, no Metal validation message on LAS, SMO, ACNH, MK8D. |
| Golden harness | see the integrator file (zero gfx warnings / validation on the default settings). |

## Not done / limits
- MetalFX temporal (no title supplies depth + motion vectors).
- iOS (out of scope). Studio / packaging are the tie-up session's.
- ACNH at 21:9 fill and "tell the game the window aspect" were only screenshotted, not judged frame by frame in play.
- LAS has a known start-up flake (guest Break in the first second); `render_scale.py` retries it.
