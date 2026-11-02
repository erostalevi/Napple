---
branch: claude/acnh-race-atomics-2026-11-01-r3
orchestration: acnh-race-atomics
base: 465077d (claude/sched-applets-2026-11-01-r1)
head: f407590
env: mac
date: 2026-11-01
areas: [crates/napple-rewrite, runtime/kernel (threads.c, signals.c), tools/soak, docs]
status: ready
---

# claude/acnh-race-atomics-2026-11-01-r3: ACNH base start crash fixed (guest spinlocks need acquire ordering on Apple cores)

## Summary
- Root cause found and fixed generically. ACNH 1.0.0's parallel-for item claim (`main+0x19b65d4`) is a spinlock that acquires with a plain
  `ldxr`/`stxr` (no acquire) and then reads the protected `next`/`total`. Apple cores run those loads before the store-exclusive completes, so two
  threads claimed the same items and recorded the same view's command buffer at once (the overlap and the `main+0x1b61d3c` crash). A Cortex-A57
  practically never reorders this way.
- Fix in `crates/napple-rewrite` (`ordering.rs`): every exclusive pair gets acquire/release ordering in place (`ldaxr`/`stlxr`, one bit). On by
  default. **Title data must be rebuilt** for it to apply (the runtime is unchanged by the fix).
- Evidence: a host litmus test that runs the exact guest lock gives thousands of double claims with `ldxr` and 0 with `ldaxr`. A new
  arbitration trace at a real collision shows the kernel sync path (LightEvent, MessageQueue svcs) behaving correctly. A new unit test of the SDK's
  LightEvent protocol passes.
- Acceptance: ACNH base 40/40 clean (was 11/40 with overlaps), update+DLC 10/10, LAS/SMO/MK8D 60 s ok.

## Files
`git diff --stat 465077d..HEAD`:
- `crates/napple-rewrite/src/ordering.rs` (new), `a64.rs`, `image.rs`, `scan.rs`, `types.rs`, `rewrite.rs`, `check.rs`, `lib.rs`
- `runtime/src/threads.c` (NAPPLE_ARB_TRACE; diagnostics only, off by default), `runtime/src/signals.c` (flush on crash)
- `runtime/tests/host/test_light_event.c` (new), `runtime/Makefile` (one appended host-test rule)
- `tools/soak/arbtrace.py`, `tools/soak/litmus_excl_claim.c` (new)
- Docs: `docs/archive/changes/2026-11-01-claude-acnh-race-atomics-2026-11-01-r3.md` (new), `docs/todo.md` (ACNH section), `docs/architecture/rewrite-format.md` (one bullet)

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| `cargo test -p napple-rewrite` | verified | all pass; new `ordering::tests` 3/3 (ACNH lock pattern, ordered/pair forms, lone/data words) |
| `make -C runtime GFX=napple test`, `make -C runtime test-host` | verified | rc 0; `light_event` 3/3, `threads_preempt` 12/12, `kernel_sched` 24/24 |
| Root cause on hardware | verified | `clang -O2 tools/soak/litmus_excl_claim.c` x4: 5333 / 24505 / 8171 / 18116 double claims (ldxr); `-DACQ` x3: 0 |
| Kernel sync ruled out | verified | `NAPPLE_ARB_TRACE` at a real collision (459523.005886): every main WaitForAddress success has its SignalToAddress; see change note |
| Title data rebuilt | verified | `/Users/neto/NappleWork/games-r3/` (ACNH, LAS, SMO, MK8D) built with this branch's CLI; ACNH main: 4267 words changed, all ordering upgrades; sdk 325 |
| Acceptance 3: ACNH base 40 x 30 s | verified | 40 ok, 0 crashes, 0 CBTRACE overlap dumps (games-r3 data, this runtime) |
| Acceptance 3: ACNH update+DLC 10 x 30 s | verified | 10 ok, 0 dumps |
| Acceptance 3: LAS / SMO / MK8D 60 s | verified | ok / ok / ok (3547 / 3548 / 3308 frames) |

## Unbuilt / untested list (the integrator checks these first)
None unbuilt. `NAPPLE_ARB_TRACE` was run on ACNH (and produced the trace in the change note); the crash-time flush in `signals.c` was not hit by a
crash in this session (compiled only).

## Findings
- The join of ACNH's job manager is `nn::os::WaitLightEvent` on per-worker done events, and workers wait on an nn::os MessageQueue. The `ldr wzr`
  loads that r1 flagged are a release-build assertion, not a wait.
- The same reordering applies to every guest exclusive-pair lock, so all titles benefit: SMO main+sdk and others now carry `ldaxr`/`stlxr`. Cost:
  ordering on exclusive pairs only (no plain loads/stores touched); no fps change seen in the 60 s boots.
- Outside my area (not fixed): the AArch32 lifter (`runtime/src/a32jit`) maps `ldrex`/`strex` to plain `ldxr`/`stxr` (docs/architecture/aarch32.md);
  32-bit titles have the same exposure and should get `ldaxr`/`stlxr`.
- Decision (user away): strengthen every exclusive pair (load→acquire, store→release) rather than only loads followed by protected reads. It is
  simpler, always correct, and the cost is small.

## How to integrate
- Land after `claude/sched-applets-2026-11-01-r1` (this branch is on top of it). No conflicts expected outside `docs/todo.md` (ACNH section only) and
  `runtime/Makefile` (one appended rule after `test_kernel_sched`).
- Build: `cargo build --release -p napple-cli`; runtime as usual (`make -C gfx && make -C gfx adapter && make -C runtime GFX=napple`).
- **Rebuild title data** (napple-latest / Studio apps) so the new rewrite applies; the shared `/Users/neto/NappleWork/games/` was not touched
  (games-r3 holds this branch's data).
- No Linux-only calls. `ordering.rs` is pure Rust.

## How to test
1. `cargo test -p napple-rewrite`; `make -C runtime GFX=napple test`; `make -C runtime test-host` (look for `light_event ... "failed":0`).
2. `clang -O2 -o /tmp/claim tools/soak/litmus_excl_claim.c && /tmp/claim` (non-zero "claimed twice" on Apple silicon), `-DACQ`: 0.
3. Rebuild ACNH (`napple build ... --data-only`), then `NAPPLE_GFX_CBTRACE=1 NAPPLE_GOLDEN_DATA=<data> python3 tools/soak/soak.py run acnh-base --runs 20 --seconds 30 --lockf <run lock>`: expect `cb=0` on every run (old data: about a third of runs had dumps).
4. Regression: LAS/SMO/MK8D 60 s with rebuilt data: ok.

## Fix policy hints
- Safe to fix directly: typos, test-harness paths. If the rewrite verifier rejects a title because of an exclusive word in data (an encoding false
  positive), the pass already skips the scanner's provably-data words; set `RewriteOptions::order_exclusives = false` for that module and report it.
  Do not remove the pass.

## Needs from other areas
- AArch32 lifter: emit `ldaxr`/`stlxr` for `ldrex`/`strex` (same race class on 32-bit titles).
