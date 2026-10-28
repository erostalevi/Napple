# 2026-10-28 runtime-small-2 (batch 2026-10-27-b7, linux-cloud)

Small portable runtime gaps. On inspection most of the listed items were already implemented on the base (`6f70b92`); this session closed what was still open and recorded the rest honestly. Everything is **tested, not verified** (Linux cloud, no Mac).

## Done
- **(6) `test_timescale` jump check**: the fixed 2 ms threshold on the largest guest-clock step across 2000 speed changes is replaced by a measured bound: each step may be at most 7.5x the real time (`CLOCK_MONOTONIC`) that passed between the two reads, plus 100 us. A host preempted for milliseconds no longer fails it. Tested on Linux (54 of 55 checks pass there; the one failure, "100 ms real at 2x = ... ticks", is the Linux host stub of `runtime_guest_ticks`, unrelated and pre-existing in this harness).
- **(4) audin permission flow hook**: `napple_audin_set_permission_hook(fn, user)` (audio.h, `audin_source.c`). Called before the `mic` capture unit starts; 0 makes `napple_audin_install_source_spec("mic")` fail with the new `NAPPLE_AUDIO_E_DENIED` (-8) and the guest keeps recording silence. NULL (default) = no check. Capture default unchanged (silence unless `NAPPLE_AUDIN_SOURCE`). Test in `audio/tests/test_audin_source.c` (+3). **Not wired**: nothing in the frontend/runtime installs a hook yet (frontend is not mine); Mac check: install a hook that calls `AVCaptureDevice requestAccessForMediaType:` and run with `NAPPLE_AUDIN_SOURCE=mic`.
- **(8) Linux `ipc` fuzz target**: it no longer linked (`decoder_vt.c`, `media_gfx_glue.c` pulled in the macOS media/gfx libs). They are now left out of `FZ_SERVICES` in `tests/fuzz/fuzz.mk`. `ipc_mut` builds; seeds + 1,068,328 mutations (seed 7, 90 s) found no crash; `make fuzz-regress` all green. Needs `libclang-rt-dev` (and `libopus-dev` for the Opus path).
- **(7) tests for the uncalled core API**: already present on the base (`tests/test_svc_unused_core.c`, commit 9b5ee2a, 28 checks: all eight functions); nothing to add. The glue-up note's list item 9 is stale.

## Already implemented on the base, nothing to add (and why)
- **(1) audren performance metrics**: header (format 1 0x10 / format 2 0x20), entries, details, history and tests exist (`audren.c` write_performance, `test_audren.c`). The task asks to check them "against the documented structure"; the documentation (yuzu / Ryujinx sources) is not reachable (raw.githubusercontent.com returns 404 for both) and this repo has no copy, so the layout stays "recalled, unverified" (comment in `audren.c`). Blocked on a reference; Mac/real-title check: a title that reads the performance frame back.
- **(2) GameCube lifo trigger / Lark-NES bits**: `napple_hid_push_gc_trigger`, `napple_hid_set_lark_type`, style bits 5..9, tests in `input/tests/test_styles.c` (31 checks) exist and pass. Offsets (0x4C20, 17 entries of 0x18) remain "recalled, unverified".
- **(3) IR camera tera / pointing (DPD)**: software model exists in `irs_core.c` (`ir_tera_compute`, `ir_dpd_compute`), commands 310/312 are `IPC_IMPL`, tests `test_svc_irs*.c`. The tera plugin firmware is not and cannot be simulated; real hardware behaviour needs a console.
- **(5) bcat layouts / storage**: `bcat_core.[ch]` has the 0x200 progress record, 0x38 directory entry, name rules, MD5 (RFC 1321 vectors) and a folder provider; 58 checks. Layouts remain from switchbrew memory.

## Verified here (Linux cloud: tested, not verified)
- `sh runtime/tests/linux/run.sh`: 47 built and ran, 0 failed (applet*, audio, fs, fs_dataid, time do not build on Linux, as before); `python3 -I runtime/tests/linux/check_known_calls.py`: 705 rows, 0 failed.
- `make -C audio -f Makefile.linux test`: all suites green (audin_source 47 checks).
- `make -C input -f Makefile.linux test`: all green.
- `make -C runtime fuzz-regress` and the `ipc` fuzz run above.
- Not run: `make -C runtime test test-services test-host` (macOS-only objects), the Mac `make -C audio test` / `make -C input test`.

## Files
`runtime/tests/test_timescale.c`, `runtime/tests/fuzz/fuzz.mk`, `audio/include/napple_audio.h`, `audio/src/audin_source.c`, `audio/tests/test_audin_source.c`.
