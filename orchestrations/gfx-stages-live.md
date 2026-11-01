---
name: gfx-stages-live
batch: 2026-11-01-r1
branch: claude/gfx-stages-live-2026-11-01-r1
base: orchestrate/2026-11-01-r1
env: mac
model: claude-opus-5-5
depends_on: []
status: handed-off
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_cfe1eb24-e8f8-4de9-9612-8b8e842af693
launched_at: 2026-11-01
---

# gfx-stages-live: transform feedback, geometry and tessellation verified on real titles

## Goal
Close `gpu_fixed/transform feedback`, `gpu_fixed/geometry shaders`, `gpu_fixed/tessellation` (+0.30 pts, plus most of the gpu_fixed −0.04 penalty):
1. Run `tools/gpu/scan_stages.py` over the four Testing Material titles (base + update + DLC) to list which use GS / tess / XFB shaders.
2. For each hit, run headless to the scene that binds them (`NAPPLE_GFX_CMDTRACE=1`, `NAPPLE_DRAWLOG_RT_ALL=1`), confirm the stage pipeline is built and drawn, compare a captured frame with expectations, fix what is wrong.
3. Read tess parameters and XFB layouts from the real control sections (offsets currently unproven), indirect-count draws with stage programs (`do_draw_indirect_count`).
4. If no title of the four uses a stage: say so, strengthen synthetic coverage with real control-section blobs dumped from the titles, and mark the items `tested` (not verified).

## Owned paths
`gfx/src/mtl_stages.inc`, `gfx/src/api_cmd.c`, `gfx/src/stages.h`, `gfx/src/shader/` (whole), `gfx/src/mtl_shader.m`, `gfx/tests/{t_stages.c,t_xlate_stages.inc,t_control.c}` and new stage tests, `tools/gpu/`,
`docs/archive/changes/2026-11-01-claude-gfx-stages-live-2026-11-01-r1.md`.

## Do not touch
`gfx/src/{mtl_replay.m,mtl_tex.m,api_texture.c,api_program.c,mtl_present.m}` (owned by gfx-msaa-views; needed edits go to "Needs from other areas"), `runtime/`, `orchestrations/`.

## Starting points
`docs/architecture/gfx.md` section 13 (stages contract), `docs/guides/gpu-title-scan.md`, `docs/archive/changes/*gfx-stages-complete*`, `*gfx-stages-real*`, `gfx/tests/t_stages.c`.

## Acceptance
1. `make -C gfx test` passes on the Mac.
2. Scan results table (title, shader hash, stage kinds) in the hand-off.
3. Verified on a real title for each stage kind the four titles use (frame capture + draw log lines); tested-only for kinds no title uses.
4. No regression: LAS, SMO, MK8D, ACNH headless boots (60 s each, under the run lock) render as before.

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data, locks, local-only, never ask).

## Result
