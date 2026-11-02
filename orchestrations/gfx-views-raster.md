---
name: gfx-views-raster
batch: 2026-11-02-r4
branch: claude/gfx-views-raster-2026-11-02-r4
base: orchestrate/2026-11-02-r4
env: mac
model: claude-opus-5-5
depends_on: []
status: launched
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_cfe1eb24-e8f8-4de9-9612-8b8e842af693 (reused)
launched_at: 2026-11-02
---

# gfx-views-raster: stencil cube sampling, remaining integer views, separate-texture slot masks, viewport swizzles, provoking vertex

## Owned paths
`gfx/src/shader/`, `gfx/src/{mtl_tex.m,mtl_replay.m,mtl_shader.m,api_cmd.c,api_state.c}`, `gfx/tests/`, `docs/architecture/gfx.md` (append), own change note.

Ownership extension approved by the orchestrator (2026-11-02): appended edits to `gfx/src/mtl_priv.h` (pipe_key fields), `gfx/src/mtl_stages.inc` (`stage_key_masks`), `gfx/src/obj.h` (two opcodes before `OP_COUNT`, enum only).

## Do not touch
`gfx/src/obj.h` layouts (keep new state in the command stream; `NVN_MIN_*` + `t_obj_canary` must stay green), `gfx/src/nvnapi/`, `runtime/`, `orchestrations/`.

## Acceptance
1. Stencil sampling of cube / cube-array depth-stencil textures translates (no `fail()` at `maxwell_translate.c:3289`), test.
2. Integer 1D / 1D-array / cube-array / 2D-MS-array views declared as integer types (`t_int_tex`).
3. Separate textures get the depth/stencil/integer slot masks (r3 finding).
4. SetViewportSwizzles applied, tested.
5. SetProvokingVertexMode first/last honoured for flat outputs (list + strip test).
6. `make -C gfx test` green; LAS corpus 5767/5767/5767; four-title 60 s no regression, no Metal validation errors. Items 4-5 may be handed off not-done if time runs short.

## Starting points
`maxwell_translate.c:3280-3290`, `api_cmd.c:832/875`, `docs/process/archive/integrators/claude-gfx-replay-gaps-2026-11-01-r3.md` (Findings).

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data — rebuilt in r3 with the ordering fix, never overwrite it; locks; local-only; never ask). This is planned as the LAST reachable round (snapshot 2026-11-02-5b9cf3a, 98.3%, ceiling ~98.9-99.0%).

## Result
