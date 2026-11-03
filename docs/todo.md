# Open work markers

Deferred code-level work that used to live as `TODO` comments in the source, plus small defects that still need a check on macOS.
Third-party and vendored files are not listed (for example `gfx/src/nvnapi/nvn.h` carries four upstream "TODO: reverse and define" notes from its author).
Per-title open items are in the [title notes](titles/README.md); the generated [feature support](reference/feature-support.md) page ranks the missing platform features.

| File | What | Why deferred |
|------|------|--------------|
| `runtime/src/services/hid.c` (`emulate_load`, NAPPLE_HID_TRACE only) | The ldur/ldurs and pre/post-index masks never matched (the value had bit 22 set but the mask did not cover it); they now require `opc != 0` like the unsigned-offset branch. The decode was checked against real encodings on Linux, the emulator itself only runs on Darwin. | Verify on a Mac with `NAPPLE_HID_TRACE=1` that loads of these forms are now traced instead of reported unhandled. |
| `input/src/tinyjson.h`, `frontend/src/tinyjson.h` | Identical copies kept in sync by a `cmp` in `input/Makefile`; one shared header would remove 200 duplicated lines. | The fix touches the Makefiles and the include paths of both libraries. |
| `runtime/src/services/fs.c` (833 lines), `applet.c` (474 lines) | Split into smaller files. | No Linux build exists for `fs.c` (needs macOS headers), so a split can only be tested on macOS. |
| `runtime/tests/test_svc_time.c` (linux-check) | Fails to link on Linux: `time.c` needs `host_symbols_register`, which `tools/linux-check/ipc_mock.c` does not provide. | A one-line mock addition in the Linux harness. |
| AArch32 lifter (`ldrex` / `strex`) | The A64 rewriter gives every exclusive pair acquire/release ordering (Apple cores run loads protected by a plain `ldxr/stxr` spinlock early); the AArch32 lifter still emits plain `ldxr/stxr`. | No 32-bit title has shown the race yet; see [rewrite format](architecture/rewrite-format.md). |
