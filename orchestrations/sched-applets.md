---
name: sched-applets
batch: 2026-11-01-r1
branch: claude/sched-applets-2026-11-01-r1
base: orchestrate/2026-11-01-r1
env: mac
model: claude-opus-5-5
depends_on: []
status: planned
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: none
launched_at: none
---

# sched-applets: fix the guest double-job scheduler race; drive swkbd and web applets live

## Goal
1. **Scheduler race (core, importance 3).** ACNH base starts show two guest threads running the same job (`main+0x1ae75c0` binds a stack context into a shared per-object slot); about 7% of base starts end in guest null reads (`main+0x1b61d3c`, `main+0x19d04d4`, `main+0x667178`). Hardware keeps them apart, so a Horizon sync or scheduling semantic is wrong here (candidates: priority/core-affinity scheduling, `svcWaitSynchronization` / `ArbitrateLock` / `SignalProcessWideKey` / condvar wake order, `svcSleepThread(0/-1/-2)` yield semantics, timer-slice preemption inside a guest critical section). Find it with `NAPPLE_GFX_CBTRACE=1` and the existing traces, fix it generically (no per-game patch), prove it with a soak.
2. **Applets live.** `applets/swkbd (software keyboard)`: inline mode header/sub/guide text and a real guest drive (a title that opens swkbd, e.g. ACNH naming or MK8D, scripted answers via the applet UI hook). `applets/web / offline web`: resolve a real title's `html/` (offline manual) layout and open it.

## Owned paths
`runtime/src/{kernel.c,thread.c,threads.c,dispatch.c,ksession.c,kmsg.c,kcode.c,signals.c,timescale.c,hle_stack.c}`, `runtime/src/services/{applet.c,applet_ae.c,applet_core.[ch],applet_lib_core.[ch],applet_web_core.[ch],applet_album_core.[ch],applet_ui.h}`,
`frontend/src/` (applet sheets), their tests, `tools/soak/`, `docs/archive/changes/2026-11-01-claude-sched-applets-2026-11-01-r1.md`. `docs/todo.md`: you may update the "ACNH start flake" section only.

## Do not touch
`gfx/`, `runtime/src/services/` other than applet*, `runtime/src/movie.c`, `audio/`, `input/`, `orchestrations/`.

## Starting points
`docs/todo.md` (ACNH start flake), `docs/archive/changes/2026-10-28-claude-gfx-recorder-flake-2026-10-28-b8.md`, `docs/architecture/runtime-services.md` (kernel/scheduler, applets), `tools/soak/`, `gfx/tests/t_cmdshare.c`.

## Acceptance
1. `make -C runtime test` passes.
2. Race: root cause written down with evidence; ACNH base 40 × 30 s headless starts (under the run lock, one at a time) with 0 guest null reads and 0 overlapping-recorder warnings, versus 3/40 before; update+DLC 10 starts still 0. LAS/SMO/MK8D 60 s boots unaffected.
3. swkbd verified with a real title (log of the inline/normal call with header texts and the scripted answer returned to the guest).
4. web applet: a real title's offline html resolved and opened (or a documented reason none of the four titles has one; then tested-only).

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data, locks, local-only, never ask). The 40-start soak is long: keep each run ≤ 30 s and release the run lock between runs so other sessions can interleave.

## Result
