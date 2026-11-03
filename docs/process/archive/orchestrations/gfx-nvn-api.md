---
name: gfx-nvn-api
batch: 2026-11-01-r2
branch: claude/gfx-nvn-api-2026-11-01-r2
base: orchestrate/2026-11-01-r2
env: mac
model: claude-opus-5-5
depends_on: []
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_c89e1641-2d09-4870-9b87-c73343780cea (reused gfx-msaa-views r1 session)
launched_at: 2026-11-01
---

# gfx-nvn-api: the 14 missing NVN entry points, refused MSAA branches, ACNH program-hash warning

## Goal
%Analysis (snapshot 2026-11-01-ae99ac1) found 14 public NVN names the titles look up that `gfx/src/nvnapi/names.txt` (the scan's catalog) lacks, and three MSAA branches that are refused. Expected +0.45 pts.

## Owned paths
`gfx/src/nvnapi/` (names.txt, nvn_protos*.h), `gfx/tools/gen_nvn_tables.sh`, `gfx/src/{api_cmd.c,api_device.c,api_program.c,api_texture.c,api_state.c,api_window.c,core.c,mtl_replay.m}`,
`tools/gen_a32_nvn.py`, `runtime/src/a32/a32_nvn_sigs.inc` (regenerate only), new `gfx/tests/t_nvn_ext*.c` and `t_msaa_*`, `docs/archive/changes/2026-11-01-gfx-nvn-api-2026-11-01-r2.md`. `gfx/Makefile` test list: append only.

## Do not touch
`gfx/src/shader/`, `mtl_shader.m`, `mtl_tex.m`, `mtl_priv.h`, `api_memory.c`, `dirty.c`, `tex_xfer.c`, `mtl_stages.inc`, `mtl_present.m` (gfx-shader-views owns most of these: ask through "Needs from other areas");
`runtime/src/{kernel,thread,threads,dispatch,ksession,kmsg,kcode,signals,timescale,hle_stack}.c`, `runtime/src/services/applet*.c`, `frontend/src/`, `tools/soak/` (sched-applets, still running); `orchestrations/`.

## Starting points
ACNH/MK8D warnings in `/Users/neto/NappleWork/logs/gfxmsaa-new-*/run.log`; `api_texture.c:216` (`nvnTextureGetViewOffset`, model for the builder variant); `api_cmd.c:970-977` (downsample/resolve); `gfx/Makefile:40` (names.txt -> tables generator);
`docs/process/archive/integrators/claude-gfx-msaa-views-2026-11-01-r1.md`.

## Acceptance
1. All 14 names in names.txt with real `NVN(...)` implementations, added **in the same commit as their names** (the catalog ratio must never drop):
   `nvnCommandBufferBind{Images,SeparateSamplers,SeparateTextures,StorageBuffers,Textures,UniformBuffers}Deferred`, `nvnCommandBufferOverrideSubtileSize`, `nvnProgramGetSubtileSize`, `nvnCommandBufferSetControlMemoryCallbackEnabled`,
   `nvnCommandBufferSetProvokingVertexMode`, `nvnCommandBufferSetZCullZF32CompressionEnable`, `nvnDeviceSetAutomaticCheckpointsMask`, `nvnDeviceWaitForError`, `nvnTextureBuilderGetViewOffset`. Tests for each.
2. a32 signatures regenerated; MK8D no longer logs "no host implementation or signature for nvnDeviceWaitForError".
3. ACNH, SMO, MK8D boots: no `NVN lookup of unknown name` lines (verified).
4. Refused MSAA branches implemented with Metal readback tests: `mtl_replay.m:2582` (MS/depth/3D copy with different size or format), `:2749` (integer Downsample across formats / arrays), `:2810` (depth<->float Downsample).
5. ACNH "program hash 0x3 is not a shader hash": explained; fixed if a host bug, otherwise documented (likely the guest double-job race, in flight in sched-applets).
6. Four-title 60-90 s boots: exit 0, 0 skipped draws, no new warnings.
SetProvokingVertexMode may need a translator key bit (translator owned by gfx-shader-views): record the mode, log the unsupported case once, request the bit in "Needs from other areas".

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data, locks, local-only, never ask).

## Result
Integrated 2026-11-01: Clean, merged into main as f0a7b07 (branch head 946e770). gfx tests all pass (t_nvn_ext, t_nvn_ext_prog, t_msaa_copy under the Metal debug layer); four-title 60 s boots: 0 skipped draws, NVN unknown-name / program-hash lines gone (ACNH 27 -> 0, MK8D 4 -> 0).
Integrator file: `docs/process/archive/integrators/claude-gfx-nvn-api-2026-11-01-r2.md`.
