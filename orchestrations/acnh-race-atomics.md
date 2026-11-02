---
name: acnh-race-atomics
batch: 2026-11-01-r3
branch: claude/acnh-race-atomics-2026-11-01-r3
base: claude/sched-applets-2026-11-01-r1 (its scheduler changes and diagnostics; lands after it)
env: mac
model: claude-opus-5-5
depends_on: [sched-applets]
status: handed-off
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_3b3cd256-1668-4a2b-8542-c3387e1fbb05 (reused sched-applets r1 session)
launched_at: 2026-11-01
---

# acnh-race-atomics: find the real cause of the ACNH base job-system collision (guest atomics / exclusive monitor / sync primitives)

## Goal
sched-applets (r1) showed the ACNH base command-buffer overlap (11 of 40 starts; guest main 0x51 vs workers 0x57/0x58 in the same parallel-for job path) is NOT scheduling: it happens with preemption off. On hardware the job system keeps them apart, so a generic CPU or kernel semantic must differ. Prime suspects, in order:
1. **Exclusive monitor emulation in the AOT rewrite** (`crates/napple-rewrite/src/{windows.rs,templates.rs,a64.rs}`, CPU %Analysis item "exclusive monitor windows (LDXR/STXR)"): a STXR that succeeds after another core wrote the location, LDAXR/STLXR ordering (acquire/release lost), CASAL/LDADDAL (LSE) forms, CLREX, a window split across a branch, 128-bit LDXP/STXP.
2. **svcArbitrateLock / svcArbitrateUnlock / svcWaitProcessWideKeyAtomic / svcSignalProcessWideKey / svcWaitForAddress / svcSignalToAddress** semantics (owner tag, waiter bit 0x40000000, signal counts, wake order, spurious wake).
3. Memory ordering on Apple silicon: DMB/DSB/ISB translated, LDAR/STLR kept, host code between guest windows that reorders.
Find which one with targeted traces and minimal guest-pattern unit tests; fix it generically (no per-game patch); prove with the soak.

## Owned paths
`crates/napple-rewrite/` (whole), `runtime/src/kernel.c` sync svcs, `runtime/src/{thread.c,threads.c,kcode.c,signals.c,hle_stack.c}`, `tools/soak/`, their tests, `docs/todo.md` ACNH start flake section, `docs/archive/changes/2026-11-01-claude-acnh-race-atomics-2026-11-01-r3.md`.
If the rewrite output format changes, title data must be rebuilt: build into `/Users/neto/NappleWork/games-r3/` (never overwrite the shared `games/`), under the build lock.

## Do not touch
`gfx/`, `runtime/src/services/`, `audio/`, `input/`, `frontend/`, `orchestrations/`.

## Starting points
sched-applets' change note `docs/archive/changes/2026-11-01-claude-sched-applets-2026-11-01-r1.md`, its integrator file, `docs/todo.md` (ACNH start flake), `NAPPLE_GFX_CBTRACE=1`, `tools/soak/soak.py --lockf`, `docs/architecture/rewrite-format.md`.

## Acceptance
1. Root cause identified with evidence (trace showing the broken atomic/sync step), or each suspect ruled out with evidence.
2. If found: generic fix + unit test reproducing the guest pattern; `cargo test -p napple-rewrite`, `make -C runtime test` green.
3. Soak: ACNH base 40 × 30 s with 0 collisions/null reads (was 11/40); update+DLC 10 starts 0; LAS/SMO/MK8D 60 s boots unaffected (all under the run lock).

## Env notes
Read `orchestrations/ENV-MAC.md` (shared title data, locks, local-only, never ask).

## Result
