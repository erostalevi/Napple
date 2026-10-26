# Open work markers

Deferred work that used to live as `TODO` comments in the source, plus defects found by the 2026-10-26 slim pass that need a Mac to settle.
Third-party and vendored files are not listed (for example `gfx/src/nvnapi/nvn.h` carries four upstream "TODO: reverse and define" notes from its author).

| File | What | Why deferred |
|------|------|--------------|
| `gfx/src/mtl_tex.m` (BC texture upload path) | Copy the auxiliary block-grid texture back into the BC texture before it is sampled. | Needs Metal on a Mac to test; Obj-C is outside the portable-code pass. |
| `runtime/src/services/hid.c` (`emulate_load`, NAPPLE_HID_TRACE only) | The ldur/ldurs and pre/post-index masks never matched (the value had bit 22 set but the mask did not cover it); they now require `opc != 0` like the unsigned-offset branch. The decode was checked against real encodings on Linux, the emulator itself only runs on Darwin. | Verify on a Mac with `NAPPLE_HID_TRACE=1` that loads of these forms are now traced instead of reported unhandled. |
| `input/src/tinyjson.h`, `frontend/src/tinyjson.h` | Identical copies kept in sync by a `cmp` in `input/Makefile`; one shared header would remove 200 duplicated lines. | The fix touches the Makefiles (mac-slim) and the include paths of both libraries. |
| `runtime/src/services/fs.c` (833 lines), `applet.c` (474 lines) | Split into smaller files. | No Linux build exists for `fs.c` (needs macOS headers), so a split cannot be tested here. |
| `runtime/tests/test_svc_time.c` (linux-check) | Fails to link on Linux: `time.c` needs `host_symbols_register`, which `tools/linux-check/ipc_mock.c` does not provide. | Pre-existing; a one-line mock addition in the Linux harness. |
