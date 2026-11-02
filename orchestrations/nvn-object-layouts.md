---
name: nvn-object-layouts
batch: 2026-11-01-r3
branch: claude/nvn-object-layouts-2026-11-01-r3
base: orchestrate/2026-11-01-r3
env: mac
model: claude-opus-5-5
depends_on: []
status: launched
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_c89e1641-2d09-4870-9b87-c73343780cea (reused)
launched_at: 2026-11-01
---

# nvn-object-layouts: guest-visible NVN object structs within the SDKs' reserved sizes

## Goal
A host struct larger than the SDK's reserved size silently corrupts guest memory (the ACNH program-hash bug, fixed in 30e67b4, was one). Make every guest-visible NVN object fit, measured against the four titles' SDKs. Correctness class, importance 3 (~+0.06 pts, drops the nvn -0.005 adjustment).

## Owned paths
`gfx/src/obj.h`, `gfx/src/api_*.c`, `gfx/src/nvnapi/`, `gfx/tests/t_nvn_*`, new `gfx/tests/t_obj_canary.c`, `docs/architecture/gfx.md` (append one section), own change note.

## Do not touch
`gfx/src/mtl_*.m`, `mtl_*.inc`, `mtl_priv.h`, `gfx/src/shader/` (gfx-replay-gaps): keep field names `mtl_*` reads; if one must move, add an accessor or leave a patch in your integrator file. `runtime/` (`runtime/src/a32/a32_nvn_sigs.inc` only via `tools/gen_a32_nvn.py` if prototypes change), `orchestrations/`.

## Acceptance
1. Committed table: every guest-visible NVN object struct in `obj.h` vs the smallest reserved size across the four titles' SDKs, with the evidence method (object strides / allocations in each title's code).
2. Every host layout fits that minimum, or host-only state moves to a side table keyed by guest address (model: `prog_t` fix 30e67b4); a FITS-style assert per object against the minimum.
3. `t_obj_canary`: a canary written after each object's minimum size survives Initialize and every setter.
4. `make -C gfx test` green; four-title boots (ACNH 90 s) no new warnings, 0 skipped draws.

## Starting points
`obj.h:19` (FITS macro), `obj.h:31-154`, `api_program.c:18` (0x88 assert), `docs/process/archive/integrators/claude-gfx-nvn-api-2026-11-01-r2.md` (Findings, Needs).

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data, locks, local-only, never ask). Shared title data in `/Users/neto/NappleWork/games/` predates the rewriter ordering fix (acnh-race-atomics, being integrated); rebuilt data for it is in `/Users/neto/NappleWork/games-r3/` — use either; never overwrite either.

## Result
