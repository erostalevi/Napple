# services-verify (plan5): Mac build and test of the Linux-written services

No source changes were needed. Verified on macOS (arm64, Apple clang) at base 8b23032.

- Prerequisites `make -C input/audio/frontend/gfx` built. `make -C gfx adapter` was also needed (the runtime link wants `gfx/build/obj/adapter/gfx_napple.o`).
- `make -C runtime`: links; the only warnings in service files are two `unused function` ones in `services/hid.c` (`ret_u32_0`, `ret_u8_0`).
- `make -C runtime test`: ipc 27, fs_journal 16, hid_service 10, timescale 21 checks; all 0 failed.
- `make -C runtime test-services`: all 25 binaries `"failed":0` (caps 35, ns 57, pdm 9 are lower than Linux because of Linux-only cases; test_hid_depth 14 checks).
- Headless boots (LAS 01006BB00C6F0000, SMO 0100000000010000, 45 s): both stop with a guest SIGSEGV at `last_stage: module-init`, fault address 0x38. The runtime built from the main checkout and the Desktop/NappleLATEST runtime crash identically on LAS, so it is not caused by the service code. No `missing_ipc`, no "beyond the request bytes" line. Services are therefore not reached by a game.
