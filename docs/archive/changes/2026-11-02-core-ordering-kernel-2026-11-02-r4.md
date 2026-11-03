# 2026-11-02 core-ordering-kernel (batch 2026-11-02-r4, Mac)

Branch `core-ordering-kernel-2026-11-02-r4`. The 32-bit twin of the r3 race fix (acquire / release for every lifted exclusive, RCsc
`lda` / `stl`), svcGetInfo 19 / 27, a non-fail summary for svcExitProcess after nnMain returned, index links for the r1-r3 change notes.

## 1. AArch32 lifter memory ordering (`runtime/src/a32jit/a32lift.c`)
- `lift_excl` emits the host's acquire / release exclusives for **every** form: `ldrex`/`ldrexb`/`ldrexh` → `ldaxr`/`ldaxrb`/`ldaxrh`, `ldrexd` →
  `ldaxp`, `strex*` → `stlxr*`, `strexd` → `stlxp`, in ARM and Thumb, plain and `ldaex`/`stlex` alike. Before, only the `ldaex`/`stlex` forms
  (FL_ACQ) got them. Same reason as r3's `crates/napple-rewrite/src/ordering.rs`: a guest lock taken with a plain pair and no barrier is
  correct on the Cortex-A57 but Apple cores run the protected loads ahead of the store-exclusive (`tools/soak/litmus_excl_claim.c`). The
  interpreter's exclusives were already acquire / seq_cst (`a32_exec.c`).
- `lda`/`ldab`/`ldah` and `stl`/`stlb`/`stlh` now map to `ldar`/`ldarb`/`ldarh` and `stlr`/`stlrb`/`stlrh` (address `x20 + Rn`). They were
  `ldr; dmb ishld` and `dmb ish; str`, which is acquire / release but not RCsc: a release store followed by an acquire load (`stl` then `lda`, or
  `stlex` then `lda`) could be reordered, which AArch32's acquire / release forbids. Decision (user away): the host pair is cheaper than the
  barriers and RCsc. An unaligned `lda`/`stl` is an alignment fault on the guest's core and on the host's; the interpreter does not model
  that fault, so difftest skips unaligned `lda`/`stl` samples where the lifted code faults (as it already did for unaligned exclusives).
- MK8D base after the rebuild (`tools/build_a32jit.sh` on a private copy): 4195 host exclusives (1803 `ldaxr`, 43 `ldaxrb`, 53 `ldaxrh`, 321
  `ldaxp`, 1801 `stlxr`, 42 `stlxrb`, 53 `stlxrh`, 79 `stlxp`), 0 plain; 10757 `lda`/`stl` as `ldar*` (8604) / `stlr*` (2153), 0 `dmb ishld`.
  Native / fallback counts unchanged (4818607 native, 48495 out of line).
- New test `runtime/tests/a32jit/excltest.c` (`make -C runtime test-a32`, runs first): 47 forms (ARM, Thumb, conditional, Thumb immediate
  offsets, `lda`/`stl`), each lifted natively with the expected host instruction and no plain exclusive or barrier. Against the previous lifter it
  fails 19 of them.

## 2. svcGetInfo 19 and 27 (`runtime/src/kernel.c`)
Read from switchbrew (SVC page, InfoType table) and Atmosphère mesosphere `kern_svc_info.cpp`:
- 19 InitialProcessIdRange is `[4.0.0-4.1.0]` only (mesosphere: `ResultInvalidEnumValue` from 5.0.0 on, checked before the handle; the range moved
  to svcGetSystemInfo 2). This kernel is 22.x, so it answers InvalidEnumValue (0xF001) without the "GetInfo id not implemented" log and summary note.
- 27 IoRegionHint `[16.0.0+]`: subid other than 0 → InvalidCombination (0xE801), else the handle must be a KIoRegion → InvalidHandle (0xE401)
  for every handle an application can hold (it has no io regions).

## 3. svcExitProcess after nnMain returned (`runtime/src/kernel.c`)
The 2-callback `nn::init::Start` has no finalizer callback (the 3-callback form's is `main.c guest_exit_clean`, r2): once nnMain returns, Start ends
the process itself through svcExitProcess, which wrote a `fail` summary. Now `kernel_exit_after_nnmain` walks the guest's frame records from the
svc's lr and x29: a return address inside `nn::init::Start` and none inside `nnMain` (both found by their symbol ranges) means nnMain returned,
and the summary is `partial`, "guest exited normally (nnMain returned, ExitProcess)", last stage `exit`. Quitting from inside nnMain, from a thread
without a Start frame, or on a 32-bit title (a32_boot runs nnMain itself; its svc registers have no frame chain) still writes `fail`.

## 4. Docs
- `docs/index.md`: links for the 18 change notes of b7 and r1-r3 that the archive README does not list (the docs checker reported them as orphans),
  plus this note. `python3 tools/check_docs.py`: no orphan change notes left.
- `docs/architecture/aarch32.md`: the exclusives and barriers paragraph.

## Verified here (Mac)
| Check | Result |
|---|---|
| `make -C runtime GFX=napple test-a32` | pass: excltest 47/47; difftest ARM 11590 / Thumb 11743 samples, 0 mismatches; inline words 1033199 samples, 0 forms failed; progtest ok; no interpreter in the runtime |
| difftest, extra seeds | family `sync` ARM seeds 2, 3 (2299 / 2207 compared, 1216 / 1177 native), Thumb seeds 2, 3, 7 (11777 / 11824 / 23317): 0 mismatches |
| `make -C runtime test-host` | pass (`test_kernel_exit` 19/19) |
| `make -C runtime GFX=napple test` / `test-services` | pass (`ipc_reference` 111 fixtures, missing 0; 67 service suites) |
| MK8D base, private rebuild, 60 s headless (`soak.py run mk8d-base`, run lock) | 6 × ok: 2878, 3342, 3464, then 3535, 3440, 3372 frames; the direct run logs `a32jit: lifted code active (.../private-core-ordering-kernel/.../a32jit.dylib)` |
| MK8D base, shared r3 data (old lifter), same runtime binary, same conditions | 3 × ok: 3289, 3324, 3340 frames |
| MK8D update (A64, private copy), 60 s | ok, 3531 frames |

The first run on the freshly written 94 MB dylib was low (2878); the five after it are within r3's 3328-3566 and at or above the same-binary
baseline. My reading: a first-load cost of the new image (signature check / page-in), not the ordering. No run was slower afterwards.
The Unicorn core fuzz (`tests/a32/fuzz_all.py`) was not run: it needs `pip install unicorn` (not installed, no download made), and it tests the
interpreter, which this branch does not change.

## Files
`runtime/src/a32jit/a32lift.c`, `runtime/src/kernel.c`, `runtime/tests/a32jit/excltest.c` (new), `runtime/tests/a32jit/difftest.c` (unaligned
`lda`/`stl` skip), `runtime/tests/host/test_kernel_exit.c` (new), `runtime/Makefile` (two appended rules), `docs/architecture/aarch32.md`,
`docs/index.md` (links), this note.
