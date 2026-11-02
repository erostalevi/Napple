---
name: leftovers-a32-viewport-docs
batch: 2026-11-02-r5
branch: claude/leftovers-a32-viewport-docs-2026-11-02-r5
base: orchestrate/2026-11-02-r5
env: mac
model: claude-opus-5-5
depends_on: []
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_3b3cd256-1668-4a2b-8542-c3387e1fbb05 (reused)
launched_at: 2026-11-02
---

# leftovers-a32-viewport-docs: interpreter lda/stl ordering + alignment, per-viewport swizzles, docs orphans

## Owned paths
`runtime/src/a32/a32_exec.c`, `runtime/tests/a32jit/difftest.c` (the lda/stl skip), viewport swizzles: `gfx/src/shader/maxwell_translate.c` (vertex output), `maxwell.h` (ShaderKey), `gfx/src/mtl_replay.m` (`vp_swizzle_key`/`OP_VP_SWIZZLE`), `mtl_shader.m`, `mtl_priv.h` (append pipe_key fields), `gfx/tests/t_vp_swizzle.c`; docs: `docs/index.md`, `docs/archive/changes/README.md`, `docs/process/archive/` index links, the support matrix; own change note.

## Do not touch
`runtime/src/services/`, `runtime/src/a32jit/`, `obj.h` structs, `gfx/src/nvnapi/`, shared title data, `orchestrations/`.

## Acceptance
1. Interpreter lda/stl seq_cst + alignment fault; difftest skip removed; `make -C runtime GFX=napple test-a32` green (excltest 47/47, 0 mismatches).
2. Viewports 1-15 use their own swizzle; new `t_vp_swizzle` case with viewport 1 != viewport 0; decide for capture-only passes; `make -C gfx test` ALL PASSED; LAS corpus 5767/5767/5767.
3. `python3 tools/check_docs.py`: 0 orphans under `docs/process/archive`; support matrix regenerated.
4. Four-title boots with `MTL_DEBUG_LAYER=1`: no regression.

## Starting points
`docs/process/archive/integrators/claude-core-ordering-kernel-2026-11-02-r4.md` (Needs from other areas), `claude-gfx-views-raster-2026-11-02-r4.md` (Viewport swizzle scope).

## Env notes
Read `orchestrations/ENV-MAC.md` (shared title data rebuilt after r4 — read-only, use private copies; locks; local-only; never ask). Final planned round (snapshot 2026-11-02-8389280, 98.9%).

## Result
Integrated 2026-11-02: **Small fix** (gen_support.py flat HISTORY default; gfx.md section 13 text and a change-archive row from the author's Needs), merged into main as bcaa690 (branch head 107ecc7). test-a32 green (excltest 47/47, 0 mismatches), `make -C gfx test` ALL TESTS PASSED, LAS corpus 5767/5767/5767, check_docs OK. Per-viewport swizzles stay *tested* (no title sets them). Follow-ups: multi-viewport rasterization (gfx), verified.json note, a32_priv.h FL_ACQ comment.
Integrator file: `docs/process/archive/integrators/claude-leftovers-a32-viewport-docs-2026-11-02-r5.md`.
