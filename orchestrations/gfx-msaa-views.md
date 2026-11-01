---
name: gfx-msaa-views
batch: 2026-11-01-r1
branch: claude/gfx-msaa-views-2026-11-01-r1
base: orchestrate/2026-11-01-r1
env: mac
model: claude-opus-5-5
depends_on: []
status: planned
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: none
launched_at: none
---

# gfx-msaa-views: MSAA sample shading, MSAA texture views, depth resolve, texture views and sparse

## Goal
Close `gpu_fixed/MSAA / resolve` and `gpu_fixed/texture views / sparse` (+0.15 pts):
- `nvnProgramSetSampleShading` is stored (`api_program.c`) but never applied: apply per-sample shading on Metal (sample-rate fragment functions / `[[sample_id]]`), keyed into the pipeline.
- MSAA texture views; `ResolveDepthBuffer` as a real depth resolve instead of a recorded barrier (`api_cmd.c` is NOT yours: the replay side is in `mtl_replay.m`; if the recorder must change, ask via "Needs from other areas").
- Texture views Metal cannot express (`mtl_tex.m view_valid` falls back to the whole texture): implement copies/aliases for the common cases (format reinterpretation within a size class, layer/level subsets); sparse textures via Metal sparse heaps on Apple silicon where an NVN title asks for them, or a documented safe fallback.

## Owned paths
`gfx/src/{mtl_replay.m,mtl_tex.m,api_texture.c,api_program.c,mtl_present.m,mtl_priv.h,obj.h,mtl_scale.m}`, new tests in `gfx/tests/` (not the stage tests), `docs/archive/changes/2026-11-01-claude-gfx-msaa-views-2026-11-01-r1.md`.
`mtl_replay.m` includes `mtl_stages.inc` (owned by gfx-stages-live): do not edit the .inc.

## Do not touch
`gfx/src/{mtl_stages.inc,api_cmd.c,stages.h,mtl_shader.m}`, `gfx/src/shader/`, `runtime/`, `orchestrations/`.

## Starting points
`docs/architecture/gfx.md`, `.claude/skills/percent-analysis/verified.json` entries for MSAA and texture views, `gfx/tests/` for MSAA/resolve tests, git log of `mtl_tex.m` (999f703).

## Acceptance
1. `make -C gfx test` passes, with new tests for sample shading, MSAA views, depth resolve and the new view cases.
2. Verified on a real title where one uses MSAA/sample shading (find it with `NAPPLE_GFX_CMDTRACE=1`; MK8D and SMO are candidates): frame capture before/after.
3. No regression: LAS, SMO, MK8D, ACNH headless boots (60 s, under the run lock) and `tools/golden` captures where available.

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data, locks, local-only, never ask).

## Result
