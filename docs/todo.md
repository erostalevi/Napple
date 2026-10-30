# Open work markers

Deferred work that used to live as `TODO` comments in the source, plus defects found by the 2026-10-26 slim pass that need a Mac to settle.
Third-party and vendored files are not listed (for example `gfx/src/nvnapi/nvn.h` carries four upstream "TODO: reverse and define" notes from its author).

| File | What | Why deferred |
|------|------|--------------|
| `runtime/src/services/hid.c` (`emulate_load`, NAPPLE_HID_TRACE only) | The ldur/ldurs and pre/post-index masks never matched (the value had bit 22 set but the mask did not cover it); they now require `opc != 0` like the unsigned-offset branch. The decode was checked against real encodings on Linux, the emulator itself only runs on Darwin. | Verify on a Mac with `NAPPLE_HID_TRACE=1` that loads of these forms are now traced instead of reported unhandled. |
| `input/src/tinyjson.h`, `frontend/src/tinyjson.h` | Identical copies kept in sync by a `cmp` in `input/Makefile`; one shared header would remove 200 duplicated lines. | The fix touches the Makefiles (mac-slim) and the include paths of both libraries. |
| `runtime/src/services/fs.c` (833 lines), `applet.c` (474 lines) | Split into smaller files. | No Linux build exists for `fs.c` (needs macOS headers), so a split cannot be tested here. |
| `runtime/tests/test_svc_time.c` (linux-check) | Fails to link on Linux: `time.c` needs `host_symbols_register`, which `tools/linux-check/ipc_mock.c` does not provide. | Pre-existing; a one-line mock addition in the Linux harness. |

## ACNH start flake (host crash fixed 2026-10-28 b8; guest race open)

Headless ACNH starts failed about 10-30% of the time on the base build and about 5% on the update build (host SIGSEGV/SIGTRAP/abort in the gfx command recorder; also a guest null read at `main+0x1b61xxx`, and on the update build a guest Break at `sdk+0xc80730` after "program hash 0x3 is not a shader hash").
- Cause (gfx-recorder-flake, `docs/archive/changes/2026-10-28-claude-gfx-recorder-flake-2026-10-28-b8.md`): two guest threads record the same object's command list at once (`main+0x1ae75c0` binds a stack-allocated context into a shared per-object slot). One thread then works on the other's stack-allocated `NVNcommandBuffer` while its owner finalizes, re-initialises and finally overwrites it. The old recorder turned that into a use after free, `realloc` races and a 64-byte static scratch overflowed by large commands.
- Fixed in gfx: host parts are addressed by slot+generation and never freed; the recording state is locked; grown buffers are freed late; unrecorded commands go to a payload-sized sink. `gfx/tests/t_cmdshare.c` reproduces the misuse without a game.
- Still open (runtime): why the two threads overlap. On hardware the game must keep them apart; NVN command buffers are not thread-safe. Timer preemption makes the overlap likelier (60 x 5 s base starts: 2 with overlap at the default slice, 0 with `NAPPLE_SLICE_MS=0`). Find it with `NAPPLE_GFX_CBTRACE=1`, which dumps both threads' events and backtraces. After the gfx fix: update+DLC 0 of 40, both builds 0 of 10 under MallocScribble, base 3 of 40, all three guest null reads (`main+0x1b61d3c`, `main+0x19d04d4`, `main+0x667178`) with no recorder warning and no host heap crash. Recheck these after the runtime fix.
