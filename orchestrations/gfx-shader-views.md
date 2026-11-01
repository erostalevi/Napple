---
name: gfx-shader-views
batch: 2026-11-01-r2
branch: claude/gfx-shader-views-2026-11-01-r2
base: orchestrate/2026-11-01-r2
env: mac
model: claude-opus-5-5
depends_on: []
status: handed-off
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_cfe1eb24-e8f8-4de9-9612-8b8e842af693 (reused gfx-stages-live r1 session)
launched_at: 2026-11-01
---

# gfx-shader-views: full shader corpus of the four titles, integer textures, ACNH cube view, sparse remap

## Goal
Close `gpu_fixed/texture views / sparse` and the ACNH cube-view fallback, and run every real shader program of the four titles through the translator and Metal compiler to find and fix the `fail()` paths real programs hit. Expected +0.09 pts plus corpus findings.

## Owned paths
`gfx/src/shader/`, `gfx/src/{mtl_shader.m,mtl_tex.m,mtl_priv.h,mtl_interp.*,api_memory.c,dirty.c,tex_xfer.c}`, `gfx/src/tex/`, `tools/gpu/`, `gfx/tests/{t_xlate*,t_view_copy*,t_tex_views.m,t_sparse.c}` and new tests, `docs/archive/changes/2026-11-01-claude-gfx-shader-views-2026-11-01-r2.md`. `gfx/Makefile` test list: append only.

## Do not touch
gfx-nvn-api's paths (`gfx/src/nvnapi/`, `api_cmd.c`, `api_device.c`, `api_program.c`, `api_texture.c`, `api_state.c`, `api_window.c`, `core.c`, `mtl_replay.m`; ask for hooks via "Needs from other areas"), `mtl_stages.inc`, `mtl_present.m`, `runtime/`, `orchestrations/`.

## Starting points
`tools/gpu/scan_stages.py` + `docs/guides/gpu-title-scan.md`; existing scan JSON in `/Users/neto/NappleWork/scan-gfxstages/json`; `gfx/tests/t_xlate.m:828` (corpus mode, `NAPPLE_SHADER_CORPUS`); `api_memory.c:193` (MapVirtual); `mtl_tex.m` `view_fmt_class` / GfxAlias; archived r1 integrator files for gfx-stages-live and gfx-msaa-views.

## Acceptance
1. `scan_stages.py` extract mode writes `.bin`/`.ctl` corpus files OUTSIDE the repo (`/Users/neto/NappleWork/shader-corpus/`); `t_xlate` corpus mode runs every distinct program of all four titles (base + updates + DLC). Report per title: programs, translated, Metal-compiled; fix every `fail()` or compile failure real programs hit (or document why it cannot be).
2. The translator declares integer sampled textures (`texture2d<uint/int>` by format), so multi-channel integer views bind as copies.
3. ACNH cube view of a 2D texture works as a copy-backed view: no "view type 5 of a type 2 texture is not expressible" and no "using a dummy texture" in ACNH boots (verified).
4. Sparse: a residency change after upload (MapVirtual remap) re-uploads the affected tiles; tested in `t_sparse`.
5. MK8D "binary archive save failed ... renaming" fixed.
6. Four-title boots: no regression.

## Env notes
Read `orchestrations/ENV-MAC.md` first. RomFS dumps are ~6 GB each: extract one title at a time and delete the dump after; cap parallel Metal compiles at 3 cores.

## Result
