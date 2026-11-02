---
name: gfx-multi-viewport
batch: 2026-11-02-r6
branch: claude/gfx-multi-viewport-2026-11-02-r6
base: claude/leftovers-a32-viewport-docs-2026-11-02-r5 (per-viewport swizzles; lands after it)
env: mac
model: claude-opus-5-5
depends_on: [leftovers-a32-viewport-docs]
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_cfe1eb24-e8f8-4de9-9612-8b8e842af693 (reused)
launched_at: 2026-11-02
---

# gfx-multi-viewport: viewport arrays and scissor arrays (ViewportIndex / layer outputs) on Metal

## Goal
NVN exposes 16 viewports and scissors; shaders select one with the ViewportIndex output (vertex/tess/geometry), and Layer output selects a render-target layer. r5 left multi-viewport rasterization open ("Needs from other areas" of leftovers-a32-viewport-docs). Metal on Apple silicon supports `setViewports:count:`, `setScissorRects:count:` and `[[viewport_array_index]]` / `[[render_target_array_index]]`.

## Owned paths
`gfx/src/shader/` (ViewportIndex/Layer attribute outputs), `gfx/src/mtl_replay.m`, `gfx/src/mtl_stages.inc`, `gfx/src/mtl_shader.m`, `gfx/src/mtl_priv.h` (append), `gfx/src/api_state.c` (viewport/scissor array setters if they drop indices >0), `gfx/tests/` (new `t_multi_viewport`), `docs/architecture/gfx.md` (append), own change note.

## Do not touch
`gfx/src/obj.h` structs (enum appends only), `gfx/src/nvnapi/`, `runtime/`, `crates/`, `orchestrations/`.

## Acceptance
1. All 16 viewports, depth ranges and scissors are set on the encoder; shaders writing ViewportIndex/Layer route primitives (vertex and geometry stages; GS-emulated paths included).
2. `t_multi_viewport`: a draw split across viewports 0/1/2 and layers 0/1 reads back correctly under `MTL_DEBUG_LAYER=1`.
3. `make -C gfx test` ALL PASSED; LAS corpus 5767/5767/5767 (and run the full four-title corpus if time allows); four-title boots with the debug layer: no regression; report whether any title writes ViewportIndex/Layer (shader corpus scan).

## Env notes
Read `orchestrations/ENV-MAC.md` (shared title data read-only, locks, local-only, never ask).

## Result
Integrated 2026-11-02 into main as f57e56f (head 62c42e6), outcome **Clean** (gfx.md doc conflict resolved mechanically, change-archive row added). `make -C gfx test` ALL PASSED incl. `t_multi_viewport` under the debug layer; LAS corpus 5767/5767/5767; four-title debug-layer boots clean. No title writes ViewportIndex / Layer. Details: [integrator file](../integrators/claude-gfx-multi-viewport-2026-11-02-r6.md).
