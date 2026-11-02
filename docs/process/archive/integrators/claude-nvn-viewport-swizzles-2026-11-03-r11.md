---
branch: claude/nvn-viewport-swizzles-2026-11-03-r11
orchestration: nvn-viewport-swizzles
base: 281f4d3
head: 281f4d3
env: mac
date: 2026-11-03
areas: [gfx]
status: integrated
---

# claude/nvn-viewport-swizzles-2026-11-03-r11: per-viewport swizzles

## Summary
No code change needed: the assignment was already implemented before this round (commit 3cb0060 "per-viewport swizzles", ancestor of base). ShaderKey carries vp_swizzle + vp_swizzle_vp[15] (maxwell.h, mtl_priv.h); maxwell_vp_swizzle_msl selects by the written ViewportIndex (clamped to 15) with a run-time table; mtl_replay.m vp_swizzle_key fills the key from all 16 viewports. t_vp_swizzle already covers viewport 1 differing from viewport 0 (vertex and geometry paths). Decision: add nothing, avoid churn in shared files.

## Files
None changed (only this integrator file).

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| t_vp_swizzle, vp1 differs from vp0, debug layer | verified | MTL_DEBUG_LAYER=1 gfx/build/tests/t_vp_swizzle -> "t_vp_swizzle: ok (0 skipped)", cases incl. index 1 vp1 x=-x, x<->y, GS index 1 |
| make -C gfx test | verified | "ALL TESTS PASSED" |
| LAS corpus 5767/5767/5767 | untested | no code changed, so unchanged from base |
| four-title boots | untested | no code changed |

## Unbuilt / untested list
None.

## Findings
Orchestration item was stale relative to commit 3cb0060 (the "viewports 1-15 reuse viewport 0" gap was already closed).

## How to integrate
Nothing to merge besides this file. No ordering constraints.

## How to test
1. cd gfx && make test (expect ALL TESTS PASSED).

## Fix policy hints
n/a

## Needs from other areas
None.

## Integration result
- Date: 2026-11-03 (batch r11 integrator run, local main, unpushed)
- Merged commit: 017bc14
- Outcome: Clean
- Docs-only hand-off (assignment already implemented before the round, no source change); nothing to fix, no native port.
