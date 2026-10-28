# gfx-features (2026-10-27, batch b6): replay of transform feedback, geometry shaders and tessellation; stencil view sampled in a shader

Branch `claude/gfx-features-2026-10-27-b6` (Mac, Metal). Contract: [gfx.md section 13](../../architecture/gfx.md) ("Replay" and "Parameters the control section would carry"). Tests: `gfx/tests/t_stages.c`, `gfx/tests/t_stencil_sample.c`.

## What changed
- **NVN recording** (`gfx/src/api_cmd.c`, `obj.h`): `BindTransformFeedbackBuffer(s)`, `Begin/End/Pause/ResumeTransformFeedback`, `DrawTransformFeedback`, `SetPatchSize`, `SetInner/OuterTessellationLevels` are recorded (they were dropped with a warning / no-ops). New ops `OP_PATCH_SIZE`, `OP_TESS_LEVELS`, `OP_BIND_XFB`, `OP_XFB`, `OP_DRAW_XFB` appended before `OP_COUNT`.
- **Replay** (`gfx/src/mtl_stages.inc`, included by `mtl_replay.m`): vertex capture pass (VS with `stage_out` or xfb entries, attachment-less 1x1 pass, points, no fragment function), CPU primitive assembly (indices, base vertex, restart, strips / fans / loops / quads / adjacency / patches) + gather kernel, GS compute dispatch + restart-strip draw over `gs_idx` with a generated pass-through vertex function, TCS dispatch + `drawPatches` with the factor buffer pre-filled from the NVN default levels, transform feedback either straight from the vertex program (plain lists) or through records and a copy kernel (everything else), byte counters kept at encode time and stored to the control memory.
- **Pipelines** (`gfx/src/mtl_shader.m`): `mtl_stage_pipe_get` (capture / GS draw / tessellation pipelines, synchronous, not persisted, `pipe_key` and its on-disk format unchanged), `mtl_compute_pipe_key` (GS / TCS kernels with a full `ShaderKey`); `pipe_desc_fill` factored out of `build_pipe` (same descriptor for normal draws).
- `bind_stage` binds the extra stages: GS / TCS on a compute encoder keep the graphics driver-constant layout (storage buffer table at c0 + 0x110 + 0x100 * hardware stage), TES on the vertex side.
- **Per-program parameters** (`gfx/src/stages.h`, registry in `api_state.c`): transform feedback layout and tess eval parameters, registered by blob hash or `NVNprogram` (the GLSLC control-section offsets are unknown).
- SPH fields read by the replay: GS output topology (word 3 bits 24..27) and max output vertices (word 4 bits 0..11), TCS output control points (word 2 bits 24..31).
- Draws under `CullFace FRONT_AND_BACK` skip their triangle output on the stage path too (as `prepare_draw`).

## Decisions (made without asking)
- Capture pass keeps rasterization on: Metal requires a `void` vertex function with `rasterizationEnabled = NO`, and the translator returns `VOut` (changing it is the translator's area). With no fragment function and no attachments nothing is written.
- A geometry program without `OUT` (pass-through GS) or one that does not translate keeps the old behaviour (drawn without the geometry stage) instead of dropping the draw: no title regresses because a GS is now honoured.
- Control memory layout is Napple's own (u32 bytes[4], u32 stride[4]): the guest only hands it back to NVN.
- Triangle strips feed odd triangles with their first two vertices swapped (winding kept), as GL does for geometry input and transform feedback.
- Default tessellation domain triangle (`NAPPLE_GFX_TESS_DOMAIN=quad`), equal spacing, CCW, until the control section is read.

## Verified vs not
| Item | Status |
|---|---|
| Transform feedback, exact words of 18 captured vertices (list direct path + indexed instanced strip through the copy kernel, Pause / Resume, unwritten words untouched, nothing past the end), control memory counts, DrawTransformFeedback coverage (`t_stages`, MTL_DEBUG_LAYER=1) | tested |
| Geometry shader point -> quad by compute emulation, pixels inside / outside / extent (`t_stages`) | tested |
| Tessellation: quad patch with NVN default levels, zero levels cull, triangle patch with TCS-written levels overriding zero state levels (`t_stages`) | tested |
| Stencil-mode view sampled in a fragment program reads 0x5A as an integer; depth-mode view of the same texture reads 0.25 (`t_stencil_sample`) | tested |
| `make -C gfx lib adapter test` | tested (ALL TESTS PASSED) |
| Headless 45 s boots of LAS, SMO, ACNH, MK8D (audio off, MTL_DEBUG_LAYER=1) | see the integrator file (filled after the runs) |
| Any game using these stages | not verified: no title in the 45 s routes binds a GS / TCS / TES or uses transform feedback; the XFB layout / tess parameters of a real title need the control-section reading |

## Files
`gfx/src/api_cmd.c`, `api_state.c`, `obj.h` (ops + payloads appended), `mtl_priv.h`, `mtl_replay.m`, `mtl_shader.m`, new `mtl_stages.inc`, `stages.h`, `tests/t_stages.c`, `tests/t_stencil_sample.c`; `docs/architecture/gfx.md` section 13.
