---
name: core-ordering-kernel
batch: 2026-11-02-r4
branch: claude/core-ordering-kernel-2026-11-02-r4
base: orchestrate/2026-11-02-r4
env: mac
model: claude-opus-5-5
depends_on: []
status: planned
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: none
launched_at: none
---

# core-ordering-kernel: acquire/release for 32-bit exclusives, svcGetInfo 19/27, clean ExitProcess summary, docs links

## Goal
The A64 race fix (r3, `crates/napple-rewrite/src/ordering.rs`) has a 32-bit twin: the a32jit lifter emits plain exclusives for ldrex/strex (MK8D base is 32-bit). Plus two kernel follow-ups. Lifts the CPU (0.14) and kernel (0.10) penalties.

## Owned paths
`runtime/src/a32jit/`, `runtime/src/kernel.c`, `runtime/tests/a32jit/`, new `runtime/tests/host/test_kernel_*`, `docs/architecture/aarch32.md`, `docs/index.md` (links only), own change note.

## Do not touch
`crates/napple-rewrite/` (reference only), `runtime/src/a32/` (interpreter already acquire/seq_cst: `a32_exec.c:677-709`), `runtime/src/services/`, `runtime/src/threads.c`, `gfx/`, the shared `/Users/neto/NappleWork/games/` (rebuild MK8D privately), `orchestrations/`.

## Acceptance
1. `lift_excl` emits `ldaxr`/`stlxr` (`ldaxp`/`stlxp`) for every ldrex/strex form incl. Thumb; lifter unit test pins it; difftest + fuzz still pass.
2. MK8D base + update rebuilt privately with the new a32 lifter: 3 × 60 s runs ok, frames within noise of r3 (3328-3566).
3. svcGetInfo 19 (InitialProcessIdRange) and 27 (IoRegionHint) answered per switchbrew, host test.
4. svcExitProcess after a clean nnMain return writes a non-fail summary, test.
5. Docs checker: no orphan r1-r3 change notes (`docs/index.md`).

## Starting points
`a32lift.c:912-940`, `kernel.c:330-336` and `382-446`, `crates/napple-rewrite/src/ordering.rs`, `tools/soak/litmus_excl_claim.c`.

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data — rebuilt in r3 with the ordering fix, never overwrite it; locks; local-only; never ask). This is planned as the LAST reachable round (snapshot 2026-11-02-5b9cf3a, 98.3%, ceiling ~98.9-99.0%).

## Result
