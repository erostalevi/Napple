# linux-check

`runtime/` only builds on macOS (Mach, Cocoa, Metal, AArch64 gate code). This directory lets the portable part of
`runtime/src/services/` be checked on Linux:

* `check-syntax.sh`: `-fsyntax-only` (gcc and clang) over every `runtime/src/services/*.c` with stub headers for the macOS-only includes.
* `run-tests.sh`: builds and runs the unit tests in `runtime/tests/test_svc_*.c` against `ipc_mock.c`, a Linux stand-in for the
  parts of `ipc.c`, `kernel.c` and `log.c` that the services call. The mock copies the real framing helpers (`ipc_in`, `ipc_out`,
  `pick_buf`, `ipc_write_buf`) so output layout is exercised, but it is **not** the real IPC layer.

Run the tests under sanitizers with `ASAN_OPTIONS=detect_leaks=0 CC="gcc -fsanitize=address,undefined" tools/linux-check/run-tests.sh` (the mock never frees what it allocates, so leak detection is off).

Nothing here is linked into the runtime. Passing these checks says the C is well formed and the portable logic behaves as the tests
expect; it says nothing about the macOS build, the real IPC dispatcher or a game.

## audio: Apple microphone capture (audio/src/audin_mic.c)

`mac/TargetConditionals.h` and `audin_mic_extra.h` let the `__APPLE__` branch of `audin_mic.c` be syntax-checked on Linux (typos and signatures only):

    clang -fsyntax-only -std=gnu11 -Wall -Wextra -D__APPLE__ -D_GNU_SOURCE \
      -Itools/linux-check/mac -Iaudio/include -Iaudio/src -Iaudio/tests/linux/include -include tools/linux-check/audin_mic_extra.h audio/src/audin_mic.c
