---
branch: claude/nvdrv-public-layouts-2026-11-02-r10
orchestration: nvdrv-public-layouts
env: linux-cloud
status: integrated
---

# Integrator: nvdrv-public-layouts (2026-11-02-r10)

Branch: `claude/nvdrv-public-layouts-2026-11-02-r10` (from `main` at `d0d4be9`). Linux cloud session: nothing here was compiled on macOS; every commit
subject starts with `unbuilt:`. Change note: `docs/archive/changes/2026-11-02-nvdrv-public-layouts-2026-11-02-r10.md`. Layout table with
per-row sources and status: `docs/reference/nvdrv-layouts.md`.

## Summary
The GPU device nodes, `/dev/nvdisp-disp0/1` and the four host1x media engines now use layouts checked against public sources (libnx, NVIDIA L4T
nvgpu / tegra_dc_ext / nvhost headers, NVIDIA open-gpu-doc class headers, Linux falcon.h), used as layout facts only (no code copied). Real mismatches
were fixed (below), every documented ioctl / method block that a reachable source pins is implemented with state tracking, and the tests pin every
struct (size, offsets, command word or method offset) and round-trip it through the real `nv_ioctl` handler.

## Per device

| Device / file | What changed | Source |
|---|---|---|
| `/dev/nvhost-as-gpu` (`nvdrv_nodes_core.c`) | **Fixed** MAP_BUFFER_EX nr 4 → 6 (4 is now NotImplemented); **fixed** INITIALIZE_EX field order (flags +0, big_page_size +8), 0 = 128K default; MAP_BUFFER_EX writes the page size back, MODIFY flag (re-kind a mapping in place) | libnx |
| `/dev/nvhost-ctrl-gpu` | **Fixed** GET_GPU_TIME nr 0x14 → 0x1C; new ZBC_GET_ACTIVE_SLOT_MASK at 0x14; **fixed** GET_CHARACTERISTICS offsets (flags +0x50, classes +0x58, chip name +0x90, warps 0x80, VA bits 0x28, FBP/LTC/ROP fields); ZCULL_GET_INFO values; short GET_CHARACTERISTICS buffer → BadParameter (was NotImplemented) | libnx |
| `/dev/nvhost-gpu` | New KICKOFF_PB 0x1B, GET_ERROR_INFO 0x16, GET_ERROR_NOTIFICATION 0x17 (libnx); ENABLE/DISABLE/PREEMPT/FORCE_RESET 0x0E–0x11, EVENT_ID_CONTROL 0x12, ALLOC_GPFIFO_EX 0x18 (L4T structs, Switch nr = L4T nr − 99, a rule checked on the libnx rows); SET/GET_USER_DATA moved to type 'G' 0x14/0x15 (`nvgpu_chan_ioctl_typed`), 'H' 0x14/0x15 removed | libnx, nvgpu.h |
| `/dev/nvdisp-disp0/1` (`nvdrv_media_core.c`) | Invented ioctls 1–5 (GET_MODE, …) replaced by the tegra_dc_ext surface: SET_NVMAP_FD, GET/PUT_WINDOW, GET/PUT_CURSOR, SET_CURSOR_IMAGE, SET_CURSOR, SET_CSC, GET_STATUS + GET_VBLANK_SYNCPT (nr 9, by size), SET_LUT, CURSOR_CLIP, CMU set/get family, SET_VBLANK, GET/SET_WINMASK, GET_SCANLINE | tegra_dc_ext.h |
| NVDEC (`nvdrv_media_regs.h`, `nvdrv_media_core.c`) | **Fixed** display buffer luma/chroma 0x130/0x131 (invented) → 0x10A/0x10B; MK8D's 0x140 is H264_SET_MBHIST_BUF_OFFSET; histogram/scratch/MV/VP8/VP9 buffer methods known; **fixed** VP9 strides (word 25, u16 pair; was 20/21), VP8 stream length (word 18) and strides; status is the 14-word `nvdec_status_s` (**fixed**: width/height were in the HEVC/VP9 statistics words) | open-gpu-doc clc5b0, nvdec_drv.h |
| VIC | Methods 0x1C1–0x1CE confirmed; slot input / config struct unchanged (trace-matched, no public VIC 4.0 header) | open-gpu-doc clceb6 |
| NVJPG | Rewritten on cle7d0 methods 0x1C0–0x1C7 (was NVDEC's 0x101–0x109), app id 1/2, `nvjpg_drv_pic_param_s`, decode/encode status; decode output types / downscale / subsample / alpha / strides; encode = entropy-coded data from the caller's tables | open-gpu-doc cle7d0, nvjpg_drv.h, clc9d1 |
| MSENC | clc5b7 methods (setup 0x1C4, status 0x1C6, bitstream 0x1C7, input 0x1CD/0x1D0/0x1D1; was 0x101–0x104 = reference pictures, 0x109), `nvenc_h264_drv_pic_setup_s`, `nvenc_pic_stat_s`, planar input | open-gpu-doc clc5b7, nvenc_drv.h |
| `jpeg_core.[ch]` | `napjpeg_encode_scan`, `napjpeg_std_huff`, `napjpeg_huff_to_dht`; caller tables lacking a code fail the encode | - |

## Needs a one-line change in `nvdrv.c` (not edited here; another session owns it)
Route every type of a GPU-channel fd to the typed entry so libnx's `_NV_IOW(0x47, 0x14)` SET_USER_DATA reaches the channel:

```diff
--- a/runtime/src/services/nvdrv.c  (gpu_node_ioctl)
-    if (f->kind == DEV_GPU_CHAN && type == NVG_TYPE_CHANNEL) r = nvgpu_chan_ioctl(&f->gchan, &env, nr, data, size);
+    if (f->kind == DEV_GPU_CHAN) r = nvgpu_chan_ioctl_typed(&f->gchan, &env, type, nr, data, size); /* 'H' and 'G' (user data) */
```

Without it everything builds and the tests pass; 'G' 0x14/0x15 on a channel stay NotImplemented (as they were before: the old code answered them as
type 'H', which is not what libnx sends). Optional for the `nvdrv.c` owner: the host1x channel SET_CLK_RATE (type 0, nr 8) / GET_CLK_RATE (0x14 before
8.0.0, 0x23 after) `{u32 rate, u32 module_id}` from libnx are not answered there. `nvdrv.c`'s other channel layouts (SUBMIT, GET_SYNCPOINT,
GET_WAITBASE, SET_SUBMIT_TIMEOUT, MAP/UNMAP_CMD_BUFFER) match libnx.

## Stale text elsewhere (outside the files this branch may edit)
`docs/architecture/runtime-services.md` still describes the old layouts: line ~285–286 (NVJPG "reduced pic-setup structure", MSENC), ~293, ~298 (VP9
words 20/21, NVJPG 12-word encode setup, MSENC reduced setup), ~331 (nvdisp "proposed ABI" GET_MODE … GET_STATUS), ~373–379 (GPU nodes "proposed",
MAP_BUFFER_EX nr 4, GET_GPU_TIME 0x14). Point them at `docs/reference/nvdrv-layouts.md`. The coverage snapshot (`tools/support/data/coverage-snapshot.json`)
and the README table are the percent-analysis run's to update; the items this branch addresses are `nvdrv`: "/dev/nvdisp-disp0", "/dev/nvhost-gpu,
as-gpu, ctrl-gpu (only if NVN is bypassed)", "/dev/nvhost-nvdec, vic, nvjpg, msenc", and `media`: "JPEG (nvjpg / caps)".

## Decisions recorded
- switchbrew could not be fetched from the cloud environment (DNS / proxy refused), so it is not cited. Ioctls whose layout only switchbrew documents
  (as-gpu REMAP, ctrl-gpu debug / PMU / time correlation, channel SET_TIMESLICE) stay NotImplemented rather than being written from memory.
- INITIALIZE_EX: libnx's field order (flags first) wins over the L4T `nvgpu_alloc_as_args` order (big page first); libnx runs on hardware.
- nvdisp: tegra_dc_ext numbering replaces the invented one even though no test title opens the device; the old numbers collided with real ones.
- NVJPG decode reads the JPEG file in the bitstream buffer (SOI at or before `bitstream_offset`); a header-less scan is refused (BAD_MAGIC) instead of
  reconstructing tables from the hardware mincode/symbol fields, whose semantics no public source states. Encode reads the 176-entry AC tables as
  16 runs × 11 sizes.
- `nvdec_status_s` no longer carries Napple's width/height (they overlapped the HEVC/VP9 statistics). Nothing in the runtime read them.
- Default AS big page when INITIALIZE_EX passes 0: 128K (what GET_CHARACTERISTICS reports), was 64K.

## Tests run on Linux (this session)
- `TMPDIR=… tools/linux-check/run-tests.sh` (every `runtime/tests/test_svc_*.c`, 50 binaries ran): every nvdrv / jpeg / caps / media test passes. Failures
  that are **identical on `origin/main` d0d4be9** (checked in a separate worktree) and unrelated to this branch: 18 link failures of the Linux mock harness
  (`test_svc_applet*`, `_audio`, `_fs*`, `_guest_exit`, `_hid`, `_movie_host`, `_ngc`, `_sysconfig`, `_time`, `_vi`: undefined `host_save_dir`,
  `frontend_metal_layer`, `threads_current_id`, … in `tools/linux-check/ipc_mock.c`), and two checks: `test_svc_mii.c:35` (host user name) and
  `test_svc_nifm_audit.c:54` (system tick).
- `tools/linux-check/run-tests.sh runtime/tests/test_svc_nvdrv_nodes.c runtime/tests/test_svc_nvdrv_media.c runtime/tests/test_svc_jpeg.c`:
  216 / 293 / 43 checks, 0 failed; the same three under `ASAN_OPTIONS=detect_leaks=0 CC="gcc -fsanitize=address,undefined"`: clean.
- `tools/linux-check/check-syntax.sh runtime/src/services/nvdrv_nodes_core.c runtime/src/services/nvdrv_media_core.c runtime/src/services/jpeg_core.c`:
  clean with gcc and clang.

## Unbuilt on macOS (everything this branch touches)
`runtime/src/services/nvdrv_nodes_core.[ch]`, `nvdrv_media_core.[ch]`, `nvdrv_media_regs.h`, `jpeg_core.[ch]`, `runtime/tests/test_svc_nvdrv_nodes.c`,
`runtime/tests/test_svc_nvdrv_media.c`. Portable C only (no new includes beyond `<stddef.h>` in the tests).

## How the Mac integrator builds and tests

```bash
make -C runtime GFX=napple test test-services
```

```bash
make -C runtime test-host
```

`test_svc_nvdrv_media` prints a "skipped" line in the regular runtime build (its `guest_ptr` rejects host heap memory); `make -C runtime test-host` runs
it fully, as before. Then boot MK8D to the menu with `NAPPLE_MEDIA_TRACE=1` and check the preview video still plays (NVDEC + VIC: only names and the
status layout changed on that path) and that the run summary shows no new `nvdrv:ioctl` misses for nvhost-nvdec / nvhost-vic.

## Integration result
- Date 2026-11-02, merged into main as 52637d5 (head c6f2597, rebased by the orchestrator). No conflicts. Outcome: **Clean** (no macOS fix needed) plus one
  doc commit: an index row for `reference/nvdrv-layouts.md` in `docs/index.md` and round r10 in the change archive (both were orphans).
- The `unbuilt:` commit compiled on macOS with no new warnings. `make -C runtime GFX=napple test test-services test-ipc-reference`: rc 0 (111 / 1325 /
  missing 0); test_svc_nvdrv_nodes 216/0, test_svc_nvdrv_media 293/0, test_svc_jpeg 43/0. `make -C runtime GFX=napple test-host`: rc 0, nvdrv 29/0,
  nvdrv_media 293/0, nvdrv_nodes 216/0.
- **MK8D NVDEC + VIC still decode** (the media-live check: `NAPPLE_MEDIA_TRACE=1 NAPPLE_VIC_DUMP=…`, `15:A,19:A,…,35:A`, 80 s): 329 NVDEC executes
  (IDR first), 16 VIC lines, 512x608 output, VIC frame 120 dump is a correct Luigi Circuit preview (Mario's kart at the start line); no nvdec/vic
  error; `missing_ipc []`, stubbed only `nvdrv:13`.
- Four-title 45 s boots, offline and `NAPPLE_NET=host`: SMO, LAS, MK8D, ACNH rc 0, notify-running, `missing_ipc []`, `unsupported_svcs []`.
- No native ports (portable layout / state code). Not done here: the one-line `nvdrv.c` routing change the file asks for (another owner; listed for
  the next batch), the stale layout text in `docs/architecture/runtime-services.md` (listed), checking the cited public sources (no web access used).
