# nvdrv GPU routing (2026-11-03, r11)

Finding: the 'H'/'A'/'G' typed ioctls were already routed (`nvdrv.c` `gpu_node_ioctl`, since b8, 105 checks); the task premise was stale. Decision: no routing change.
Added (Linux-tested, 120 checks): 'H' 0x17 GET_ERROR_NOTIFICATION, 'H' 0x1D EVENT_ID_CTRL (new `NvGpuChan.event_mask`), 'G' 0x0C GET_TPC_EXCEPTION_EN_STATUS, 'G' 0x0D NUM_VSMS.
Docs: new `docs/reference/nvdrv-layouts.md` (did not exist), nvdrv rows of `docs/architecture/runtime-services.md`.
Limits: no network to switchbrew/NVIDIA headers, layouts proposed from memory; remaining refused ioctls listed with reasons in nvdrv-layouts.md.
