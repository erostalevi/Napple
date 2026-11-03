---
name: gfx-replay-gaps
batch: 2026-11-01-r3
branch: claude/gfx-replay-gaps-2026-11-01-r3
base: orchestrate/2026-11-01-r3
env: mac
model: claude-opus-5-5
depends_on: []
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_cfe1eb24-e8f8-4de9-9612-8b8e842af693 (reused)
launched_at: 2026-11-01
---

# gfx-replay-gaps: D24 readback, separate texture/sampler binds, integer 3D/cube/MS views

## Goal
Close gpu_fixed/depth/stencil formats, the 6 nvn/nvnCommandBufferBindSeparate* stubs and gpu_fixed/texture views / sparse (~+0.26 pts; %Analysis snapshot 2026-11-01-4bf94be).

## Owned paths
`gfx/src/{mtl_replay.m,mtl_tex.m,mtl_stages.inc,mtl_priv.h,mtl_shader.m}`, `gfx/src/shader/`, `tools/gpu/`, new tests in `gfx/tests/` (`t_d24_readback`, `t_sep_sampler`, `t_int_tex` extensions, `t_xlate` pins), `docs/architecture/gfx.md` (append one paragraph), own change note.

## Do not touch
`gfx/src/obj.h`, `gfx/src/api_*.c`, `gfx/src/nvnapi/` (nvn-object-layouts; API-side needs go as a patch in your integrator file), `runtime/`, `audio/`, `input/`, `crates/`, `orchestrations/`.

## Acceptance
1. D24 (0x33) / D24S8 (0x35) texture -> buffer copies write the NVN packed layout (inverse of `copy_d24_to_tex`); `t_d24_readback` round trip under `MTL_DEBUG_LAYER=1`; "reading back a 24-bit depth texture" warning gone. (Stencil sampling of cube views optional.)
2. Separate texture/sampler binds: replay combines `OP_BIND_SEPTEX`/`OP_BIND_SEPSAMP` slots into sampled bindings; translator handles a handle combined from two cbuf words; `t_sep_sampler` reads the texel expected with a non-default sampler; "separate texture/sampler binding is not supported yet" gone.
3. Integer views of 3D / cube / MS targets declared `texture3d/texturecube/texture2d_ms<uint|int>`; `t_int_tex` extended, no Metal validation error.
4. `make -C gfx test` ALL TESTS PASSED; LAS corpus still 5767/5767/5767; four-title 60 s boots (ACNH 90 s) under the run lock: 0 skipped draws, no new warnings.
5. Integrator file lists the verified.json keys to flip.

## Starting points
`mtl_replay.m:2497` + `copy_d24_to_tex`; `mtl_replay.m:3275-3284`; `api_cmd.c:742-753`, `api_device.c:83-84` (read only); integer-texture paragraph of `docs/archive/changes/2026-11-01-gfx-shader-views-2026-11-01-r2.md`; `mtl_tex.m mtl_tex_int_kind`; `maxwell_translate.c` "bindless texture handle is not a constant buffer load".

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data, locks, local-only, never ask). Shared title data in `/Users/neto/NappleWork/games/` predates the rewriter ordering fix (acnh-race-atomics, being integrated); rebuilt data for it is in `/Users/neto/NappleWork/games-r3/` — use either; never overwrite either.

## Result
Integrated 2026-11-02: Clean (ready-with-caveats), merged into main as 95f9519 (branch head b1b8556), after nvn-object-layouts (no conflict; `docs/architecture/gfx.md` auto-merged). `make -C gfx test` ALL TESTS PASSED (`t_d24_readback`, `t_sep_sampler`, `t_int_tex`, `t_xlate`, `t_obj_canary` ok); LAS corpus 5767 / 5767 / 5767; four-title boots with `MTL_DEBUG_LAYER=1` rc 0, 0 skipped draws, no validation errors or warnings. Caveat stands: no title binds separate handles, so the c2 table offsets (0x120 / 0x520) stay a hypothesis; all three features are *tested*, not *verified* on a title. verified.json flips proposed by the author are left to /percent-analysis (tested, not verified).
Integrator file: `docs/process/archive/integrators/claude-gfx-replay-gaps-2026-11-01-r3.md`.
