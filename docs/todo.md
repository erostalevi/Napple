# Open work markers

Deferred work that used to live as `TODO` comments in the source, plus defects found by the 2026-10-26 slim pass that need a Mac to settle.
Third-party and vendored files are not listed (for example `gfx/src/nvnapi/nvn.h` carries four upstream "TODO: reverse and define" notes from its author).

| File | What | Why deferred |
|------|------|--------------|
| `runtime/src/services/hid.c` (`emulate_load`, NAPPLE_HID_TRACE only) | The ldur/ldurs and pre/post-index masks never matched (the value had bit 22 set but the mask did not cover it); they now require `opc != 0` like the unsigned-offset branch. The decode was checked against real encodings on Linux, the emulator itself only runs on Darwin. | Verify on a Mac with `NAPPLE_HID_TRACE=1` that loads of these forms are now traced instead of reported unhandled. |
| `input/src/tinyjson.h`, `frontend/src/tinyjson.h` | Identical copies kept in sync by a `cmp` in `input/Makefile`; one shared header would remove 200 duplicated lines. | The fix touches the Makefiles (mac-slim) and the include paths of both libraries. |
| `runtime/src/services/fs.c` (833 lines), `applet.c` (474 lines) | Split into smaller files. | No Linux build exists for `fs.c` (needs macOS headers), so a split cannot be tested here. |
| `runtime/tests/test_svc_time.c` (linux-check) | Fails to link on Linux: `time.c` needs `host_symbols_register`, which `tools/linux-check/ipc_mock.c` does not provide. | Pre-existing; a one-line mock addition in the Linux harness. |

## ACNH start flake (open, found 2026-10-28)

Headless ACNH starts fail about 10-30% of the time on the base build and about 5% on the update build (host SIGSEGV/SIGTRAP/abort in the gfx command recorder, `EndRecording` memcpy from a garbage `h->buf`; or a guest null read at `main+0x1b61xxx`; update build: guest Break at `sdk+0xc80730` after "program hash 0x3 is not a shader hash").
- Deterministic repro on the base build: `MallocScribble=1 MallocPreScribble=1` crashes 16 of 16 starts at that `memcpy` (`h_nvnCommandBufferEndRecording+324`, `api_cmd.c`). Without scribble it is timing dependent.
- Fixed on the way (did not remove the flake): handle lookup + retain is now atomic (`cmdrec_acquire`), a copied guest command buffer object no longer shares the host part (`owner` check).
- Not the cause: gfx stages (`NAPPLE_GFX_NO_STAGES`), module unload, rewriter `x18` use. Timer preemption raises the rate (`NAPPLE_SLICE_MS=0`: update build 0/24, base 2/20). `NAPPLE_FREE_CORES=1` crashes ACNH almost always.
- Seen in logs: stack-allocated command buffers on recycled thread stacks, calls on finalized objects ("command buffer call on a null or finalized command buffer ignored"), "BeginRecording while already recording".
- Next step: run the scribble repro under a debugger / ASan build of gfx and find who frees or overwrites the `cmdbuf_host` (or `h->buf`) while it is in use.
