# Integrator note: nvdrv-gpu-routing r11

Branch `claude/nvdrv-gpu-routing-2026-11-03-r11`. Files: `nvdrv_nodes_core.[ch]`, `runtime/tests/test_svc_nvdrv_nodes.c`, the two docs, change note. `nvdrv.c` is **unchanged**.

- **Routed ioctls:** none new; routing existed already (`do_ioctl` -> `gpu_node_ioctl`), covered by the existing tests (argument size, state, error codes, wrong type/closed fd).
- **New ioctls:** 'H' 0x17 GET_ERROR_NOTIFICATION, 'H' 0x1D EVENT_ID_CTRL, 'G' 0x0C GET_TPC_EXCEPTION_EN_STATUS, 'G' 0x0D NUM_VSMS. Layouts proposed from memory (no network to switchbrew/NVIDIA headers in this session).
- **Still refused:** KICKOFF_PB, SET_TIMESLICE/WAIT/old ALLOC_GPFIFO, AS REMAP/MAP_BUFFER_BATCH, debugger/vidmem 'G' ioctls; reasons in `docs/reference/nvdrv-layouts.md`.
- **Linux evidence:** `tools/linux-check/run-tests.sh runtime/tests/test_svc_nvdrv_nodes.c` -> 120 checks, 0 failed (also under ASan/UBSan); `test_svc_nvdrv_media` 172/0 without sanitizers. With ASan `test_svc_nvdrv_media` fails, also on the unmodified base (not touched here).
- **Unbuilt:** the core changes are portable C and compiled on Linux only; `nvdrv.c` untouched, so nothing new is macOS-only.
- **Mac:** `make -C runtime GFX=napple test test-services`

## Integration result
- Date: 2026-11-03 (batch r11 integrator run, local main, unpushed)
- Merged commit: 210b461
- Outcome: Semantic conflicts resolved (keep both)
- Landed by cherry-picking its two commits (7e2c257, 1d806bc) onto main. The branch did not know the round-10 nvdrv-public-layouts work, which already implements 'H' 0x17 GET_ERROR_NOTIFICATION and an EVENT_ID_CONTROL (0x12, {cmd,event_id}); resolution: round-10 code, layouts and docs kept as authoritative; added from the branch only its genuinely new rows, as separate cases: 'H' 0x1D EVENT_ID_CTRL (own form {cmd,event_id,fd,pad}, event_id 0..2, shares the round-10 enable mask, clear keeps it), 'G' 0x0C GET_TPC_EXCEPTION_EN_STATUS, 'G' 0x0D NUM_VSMS (constants added to nvdrv_nodes_core.h), their tests (event_mask renamed to events), and a 'Round 11 additions' section appended to docs/reference/nvdrv-layouts.md (the branch's version of that file was an add/add conflict and its 'stay NotImplemented' table is superseded). Its layouts are proposed from memory, unverified (0x1D in particular may not be a real Switch number). nvdrv.c unchanged. test_svc_nvdrv_nodes 231/0, all runtime suites green.
