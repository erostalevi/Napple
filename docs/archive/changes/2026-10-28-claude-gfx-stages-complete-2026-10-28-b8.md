# gfx-stages-complete (b8, 2026-10-28)

Branch `claude/gfx-stages-complete-2026-10-28-b8`, Mac. Finishes the geometry / tessellation / transform feedback paths of `gfx/src/mtl_stages.inc`.

## What changed
- **Software tessellator** (`mtl_stages.inc`: `tessgen` / `tesslist` kernels, `tes_soft_capture`, `draw_tessellation`): isolines (GL rules), transform feedback of tess eval output, and a geometry program after tessellation. Plain triangle / quad patches keep Metal's `drawPatches`. Quads and triangles are tessellated as a uniform grid (the hardware's vertices for equal levels with integer spacing, not its ring triangulation); levels capped by `NAPPLE_GFX_SOFT_TESS_MAX` (16) when a tess control program writes them.
- **Translator**: `ShaderKey.tess_soft` runs a tess eval program as a plain vertex function over `tess_uv` (buffer 19); allows `stage_out` / xfb / `capture_void` and isolines on it; a TCS for isolines writes the quad factor record. `TRANSLATOR_VERSION` 13 -> 14.
- **Geometry**: `draw_geometry` split into `gs_decl_of` + `gs_run` (also fed by tessellation with a GPU-side primitive count). Clip-space conversions moved from the GS kernel to the pass-through vertex function (`stage_pipe_key.gs_clip`): fixes captured GS positions being flipped when the rasterizer is on (b7 bug).
- **Control section** (`shader/control.c/.h`): odd (0x7d1) and long (up to 0x11d0) section lengths kept (they fell back to 0x880, so the dump and reads ran past short sections); facts from the b8 dumps recorded in `control.h`.
- Docs: `Napple/docs/architecture/gfx.md` section 13.

## Verified / tested
- `make -C Napple/gfx test`: ALL TESTS PASSED; t_stages ok (0 skipped) with 5 new exact-word checks (TES capture ccw + cw, indirect triangle patches with a TCS at level 4, isolines drawn + captured, tess + GS drawn + captured, GPU-written control memory); t_xlate (soft TES contract), t_control (0x7d1 / 0x11d0 lengths, assembly-text pattern, fragment +0x718). Tested.
- Control-section readers over 20624 dumped sections (LAS, MK8D A32 / 64-bit, ACNH, SMO 45 s boots): all parse, no false layout / tess word. Verified. The two candidate offsets stay unproven: no title has a transform feedback layout or a tess program.
- 45 s headless boots of 4770f82 (LAS, LAS NO_STAGES, MK8D A32, MK8D 64-bit, SMO, ACNH; audio off, debug layer, gamelock): all exit 0, 0 skipped draws, no new gfx warning vs the 844ec59 base, no Metal validation lines. Verified. Draw counts on MK8D were lower in that run because the shader / pipeline caches were cold after the TRANSLATOR_VERSION bump; warm-cache re-runs show head at or above base (A32 ~274k vs ~202k draws, 64-bit ~323k vs ~320k, within noise).

## Not done
- `MultiDraw*IndirectCount` with stage programs (in `mtl_replay.m`, not owned).
- Point-mode tessellation; exact hardware triangulation of unequal levels.
- No game uses these paths, so nothing is verified in a game.
