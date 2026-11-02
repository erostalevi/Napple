---
branch: claude/core-ordering-kernel-2026-11-02-r4
orchestration: core-ordering-kernel
base: ed33737 (orchestrate/2026-11-02-r4)
head: 03aef3a (code; the docs commit on top of it adds the change note, index links and this file)
env: mac
date: 2026-11-02
areas: [runtime/a32jit, runtime/kernel, docs]
status: integrated
---

# claude/core-ordering-kernel-2026-11-02-r4: acquire/release for 32-bit exclusives, svcGetInfo 19/27, clean ExitProcess summary, docs links

## Summary
- `lift_excl` emits `ldaxr`/`stlxr` (`ldaxrb/h`, `stlxrb/h`, dword `ldaxp`/`stlxp`) for **every** ldrex/strex form, ARM and Thumb, plain and
  ldaex/stlex: the 32-bit twin of r3's `ordering.rs`. New `excltest` pins all 47 forms (fails 19 on the old lifter).
- Also in the lifter: `lda`/`stl` (+ byte/half) now lift to `ldar`/`stlr`. The old `ldr; dmb ishld` / `dmb ish; str` was not RCsc (a release store
  followed by an acquire load could reorder). MK8D has 10757 of them.
- MK8D base rebuilt privately (`/Users/neto/NappleWork/private-core-ordering-kernel`, APFS clone of the shared dir, only `a32jit.dylib` rewritten;
  shared `games/` untouched, md5 unchanged): 6 × 60 s ok; 5 of 6 within r3's 3328-3566 (first run 2878, see Findings). Update 1 × ok.
- svcGetInfo 19 → InvalidEnumValue (4.0.0-4.1.0 only; this kernel is 22.x), 27 → InvalidCombination (subid != 0) / InvalidHandle. Host test.
- svcExitProcess after nnMain returned (2-callback nn::init::Start) writes `partial` "guest exited normally" instead of `fail`. Host test.
- `docs/index.md`: links for the 18 orphaned b7 / r1-r3 change notes and this round's note.
- Not done: the Unicorn core fuzz (needs a PyPI download; it tests the interpreter, which is unchanged).

## Files
`git diff --stat ed33737..HEAD`:
- `runtime/src/a32jit/a32lift.c` (lift_excl, lift_ldst), `runtime/src/kernel.c` (GetInfo 19/27, `kernel_exit_after_nnmain`, svc_exit_process)
- Tests: `runtime/tests/a32jit/excltest.c` (new), `runtime/tests/a32jit/difftest.c` (one line: skip unaligned lda/stl that fault in lifted code),
  `runtime/tests/host/test_kernel_exit.c` (new)
- Shared, append-only: `runtime/Makefile`: excltest rule + `test-a32-order` (a prerequisite of `test-a32`, after the test-a32 block);
  `test_kernel_exit` rule + `HOST_TESTS +=` (just before `test-host: $(HOST_TESTS)`)
- Docs: `docs/architecture/aarch32.md` (exclusives and barriers paragraph), `docs/index.md` (links only),
  `docs/archive/changes/2026-11-02-claude-core-ordering-kernel-2026-11-02-r4.md` (new)
- Not touched: `crates/napple-rewrite/`, `runtime/src/a32/`, `runtime/src/services/`, `runtime/src/threads.c`, `gfx/`, shared `NappleWork/games/`, `orchestrations/`.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Acceptance 1: every ldrex/strex form → ldaxr/stlxr (ldaxp/stlxp), Thumb included, lifter test | tested | `build/tests/a32/excltest`: 47/47 (ARM, Thumb, conditional, Thumb #imm, lda/stl); old lifter 28/47 |
| Acceptance 1: difftest + fuzz still pass | tested (difftest) / not run (Unicorn) | `make -C runtime GFX=napple test-a32` rc 0: difftest ARM 11590 / Thumb 11743 compared, 0 mismatches; inline_words 1033199 samples, 0 failed; progtest ok; no-interp ok. Extra: `--family sync` ARM seeds 2, 3 and Thumb seeds 2, 3, 7: 0 mismatches. `tests/a32/fuzz_all.py` not run (unicorn missing; interpreter unchanged) |
| Acceptance 2: MK8D base + update rebuilt privately, 3 × 60 s, frames within noise | verified | `tools/build_a32jit.sh` on the private copy: 4195 exclusives all acquire/release, 0 plain. `NAPPLE_GOLDEN_DATA=/Users/neto/NappleWork/private-core-ordering-kernel soak.py run mk8d-base --runs 3 --seconds 60 --lockf ...run.lock` twice: ok 2878 / 3342 / 3464, ok 3535 / 3440 / 3372. Same binary on the shared r3 data: 3289 / 3324 / 3340. Update (A64, unchanged code) 1 × ok 3531. Direct run log: `a32jit: lifted code active (/Users/neto/NappleWork/private-core-ordering-kernel/.../a32jit.dylib)` |
| Acceptance 3: svcGetInfo 19 / 27 per switchbrew, host test | tested | `build/host/test_kernel_exit`: 19/19 (G19 ×4, G27 ×3, neighbours, classification ×7, X1/X2 ×3) |
| Acceptance 4: ExitProcess after a clean nnMain return writes a non-fail summary, test | tested | same test: forked child, ExitProcess with Start on the frame chain → `partial|guest exited normally (nnMain returned, ExitProcess)`, exit 0; from inside nnMain / no Start frame → `fail`. Not verified on a title: none of the four returns from nnMain |
| Acceptance 5: no orphan r1-r3 change notes | verified | `python3 tools/check_docs.py`: 0 `archive/changes` problems (was 18). It still reports 30 `docs/process/archive/{integrators,orchestrations}` orphans and the stale support matrix (not change notes; integrator / generator) |
| Regression | tested | `make -C runtime test-host` rc 0, `make -C runtime GFX=napple test` rc 0 (ipc_reference missing 0), `test-services` rc 0 (67 suites) |

## Unbuilt / untested list (the integrator checks these first)
None unbuilt. Not exercised on a title: the ExitProcess `partial` branch (no test title returns from nnMain), GetInfo 19 / 27 (no title calls them).

## Findings
- MK8D's 32-bit code uses acquire / release accesses heavily: 8604 `lda*` (7092 of them `ldab`, likely static-init guards), 2153 `stl*`, 4195
  exclusive halves.
- First run on the freshly written dylib: 2878 frames, the five after it 3342-3535. The same-binary baseline on the shared data shows no such dip
  (its dylib was already loaded before). My reading: first-load cost of a new 94 MB image (code signature check / page-in), not the ordering.
  Recheck on an idle Mac if a number matters.
- Decision (user away): lda / stl moved to ldar / stlr. An unaligned lda / stl faults on the host, and it is an alignment fault on the guest's
  core too. The interpreter (`runtime/src/a32/a32_exec.c`, not this branch's) does not raise that fault, so difftest skips those samples.
- Interpreter twin (not fixed, not this branch's): `a32_exec.c` lda / stl use acquire / release fences around plain accesses, which is not RCsc
  either (stl then lda may reorder). Only the debug interpreter build uses that path; the shipped runtime has no interpreter.
- `svc_break` (kernel.c) reads `r[29]`/`r[30]`. The A32 svc path passes an 8-slot array, but a32_proc.c handles Break (0x26) itself before
  dispatch, so it is not reached. The new ExitProcess check skips 32-bit titles (`memory_a32_base()`) for the same reason.
- GetInfo 19: switchbrew `[4.0.0-4.1.0]`; mesosphere gates it on `< 5.0.0` and checks that before the handle. GetInfo 27: switchbrew's handle
  column says "Process", but mesosphere requires a KIoRegion handle. I followed mesosphere (the implementation) for the checks.

## How to integrate
- No order constraints. Conflict hotspots: `runtime/Makefile` (two appended blocks; keep other branches' appended rules too), `docs/index.md`
  (one list under "Process and history"; if another branch also links notes there, keep both, or move all of them into
  `docs/archive/changes/README.md`).
- Build: `make -C runtime GFX=napple`. The lifter change reaches title data only when the a32jit dylib is rebuilt: for MK8D base,
  `tools/build_a32jit.sh <data>/0100152000022000/b5c39b5b7c62a88e`, or a full `napple build`; napple-rewrite's build.rs compiles
  `runtime/src/a32jit/a32lift.c` into the crate. The A64 data does not change.
- No Linux-only calls.

## How to test
1. `make -C runtime GFX=napple test-a32`: first line `excltest: 47/47 passed`, then difftest `mismatches 0` twice, `0 forms failed`, progtest ok,
   `ok: no AArch32 interpreter`. A `FAIL <form> <insn>` line means a form lost its ordering.
2. `make -C runtime test-host`: `{"test":"kernel_exit","checked":19,"failed":0}`.
3. `make -C runtime GFX=napple test` and `test-services`: rc 0.
4. MK8D base with a rebuilt a32jit.dylib, 3 × 60 s (`soak.py run mk8d-base --runs 3 --seconds 60 --lockf /Users/neto/NappleWork/.run.lock`
   with `NAPPLE_GOLDEN_DATA` at the rebuilt data): ok, frames ~3300-3550.
5. `python3 tools/check_docs.py | grep archive/changes`: nothing.

## Fix policy hints
- If a reviewer prefers barriers to ldar / stlr for lda / stl, the alternative is `dmb ish; str; dmb ish` for stl and `ldar` (or a leading
  `dmb ish`) for lda. A trailing barrier only on stl does not cover stlex → lda. excltest's expectations change with it.
- Test expectations and the Makefile lines are safe to adjust directly.

## Needs from other areas
- `runtime/src/a32/a32_exec.c` owner: model the alignment fault of lda / stl (then the difftest skip can go), and make lda / stl RCsc (seq_cst
  load / store instead of fences around plain accesses).
- Integrator / docs: list the b7 / r1-r4 notes in `docs/archive/changes/README.md` (they are reachable from the index now). Link
  `docs/process/archive/{integrators,orchestrations}/` (30 orphans). Regenerate the support matrix.
- Shared title data: rebuild MK8D base's `a32jit.dylib` in `NappleWork/games/` with the new lifter when this lands. Not done here: shared data is
  read-only for sessions.

## Integration result
- Date 2026-11-02, merged into main as 827afa5 (branch head 07c45c5, matches the hand-off: code 03aef3a + docs commit). Outcome: **Clean** (no `integrate:` fixes, no native ports: the lifter and kernel changes are pure logic, left as C).
- Build: `make -C gfx lib adapter`, frontend, input, audio, `make -C runtime GFX=napple`: rc 0.
- `make -C runtime GFX=napple test-a32`: `excltest: 47/47 passed`; difftest mismatches 0 (11590 ARM samples); inline_words 1033199 samples, 0 forms failed; `ok: no AArch32 interpreter`.
- `make -C runtime test-host`: `{"test":"kernel_exit","checked":19,"failed":0}`, all host suites 0 failed. `make -C runtime GFX=napple test`: rc 0, ipc_reference fixtures 111 / missing 0. `test-services`: rc 0, 0 failed.
- `python3 tools/check_docs.py | grep archive/changes`: nothing.
- MK8D base, private copy (`/Users/neto/NappleWork/games-integ-r4/`, APFS clone of the shared data with `a32jit.dylib` rebuilt by `tools/build_a32jit.sh` from this tree, under the build lock; shared `games/` untouched at that point): `soak.py run mk8d-base --runs 3 --seconds 60 --lockf .run.lock`: 3/3 ok, frames 3039 / 3348 / 3328 (first-run dip again, as in Findings; the other two inside r3's 3328-3566).
- Unverified as before: the ExitProcess `partial` path and GetInfo 19/27 on a title (no title reaches them). Unicorn fuzz not run.
- Correction to "How to test" step 4: `soak.py` reads `NAPPLE_GOLDEN_DATA/<titleid>/napple-app.json`, so the private copy must be the whole title dir, not just the build-id dir.
- Shared title data rebuilt from final main 2d3c68c after the whole batch landed (old data kept as `/Users/neto/NappleWork/games-old-20261102-r4/`): MK8D base `a32jit.dylib` now has 4195 acquire/release exclusives and 0 plain ones (`otool -tv`; the old shared dylib had 1785 plain). `soak.py run mk8d-base --runs 3 --seconds 60` on it: 3/3 ok, 3411 / 3385 / 3419 frames; `a32jit: lifted code active` from the shared path.
