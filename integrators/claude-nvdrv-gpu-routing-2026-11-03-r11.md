# Integrator note: nvdrv-gpu-routing r11

Branch `claude/nvdrv-gpu-routing-2026-11-03-r11`. Files: `nvdrv_nodes_core.[ch]`, `runtime/tests/test_svc_nvdrv_nodes.c`, the two docs, change note. `nvdrv.c` is **unchanged**.

- **Routed ioctls:** none new; routing existed already (`do_ioctl` -> `gpu_node_ioctl`), covered by the existing tests (argument size, state, error codes, wrong type/closed fd).
- **New ioctls:** 'H' 0x17 GET_ERROR_NOTIFICATION, 'H' 0x1D EVENT_ID_CTRL, 'G' 0x0C GET_TPC_EXCEPTION_EN_STATUS, 'G' 0x0D NUM_VSMS. Layouts proposed from memory (no network to switchbrew/NVIDIA headers in this session).
- **Still refused:** KICKOFF_PB, SET_TIMESLICE/WAIT/old ALLOC_GPFIFO, AS REMAP/MAP_BUFFER_BATCH, debugger/vidmem 'G' ioctls; reasons in `docs/reference/nvdrv-layouts.md`.
- **Linux evidence:** `tools/linux-check/run-tests.sh runtime/tests/test_svc_nvdrv_nodes.c` -> 120 checks, 0 failed (also under ASan/UBSan); `test_svc_nvdrv_media` 172/0 without sanitizers. With ASan `test_svc_nvdrv_media` fails, also on the unmodified base (not touched here).
- **Unbuilt:** the core changes are portable C and compiled on Linux only; `nvdrv.c` untouched, so nothing new is macOS-only.
- **Mac:** `make -C runtime GFX=napple test test-services`
