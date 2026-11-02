---
name: nvn-viewport-swizzles
batch: 2026-11-03-r11
branch: claude/nvn-viewport-swizzles-2026-11-03-r11
base: orchestrate/2026-11-03-r11
env: mac
model: claude-sonnet-5-5
depends_on: []
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: pending
launched_at: 2026-11-03
---

# nvn-viewport-swizzles: per-viewport swizzles (viewports 1-15 currently reuse viewport 0)

## Owned paths
`gfx/src/shader/maxwell_translate.c`, `maxwell.h` (append), `gfx/src/mtl_replay.m` (swizzle path only), `mtl_priv.h` (append), `gfx/tests/t_vp_swizzle.c`

## Do not touch
Everything else (other sessions edit runtime/src/services, runtime/src/net, gfx shader/replay/stages, media in parallel), `orchestrations/`.

## Work and acceptance
1. A shader writing viewport index N must apply viewport N's swizzle (positive/negative X/Y/Z/W) — carry the 16 swizzles into the pipeline key / a uniform and select by ViewportIndex in the vertex/geometry output.
2. `t_vp_swizzle`: viewport 1 differs from viewport 0 and reads back correctly under MTL_DEBUG_LAYER=1.
3. `make -C gfx test` green, LAS shader corpus 5767/5767/5767, four-title boots with the debug layer unchanged.

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data read-only, lockf run lock, local-only, never ask, never push). Model: Sonnet 5.5, medium effort: work test-first in small steps, commit each.

## Result

Outcome: Clean. Merged commit 017bc14. Docs-only hand-off (assignment already implemented before the round, no source change); nothing to fix, no native port.
Integrator record: docs/process/archive/integrators/claude-nvn-viewport-swizzles-2026-11-03-r11.md
