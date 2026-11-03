# nvdrv-public-layouts (2026-11-02-r10): GPU nodes, nvdisp and the media engines checked against public layouts

Branch `nvdrv-public-layouts-2026-11-02-r10`, Linux cloud session (the macOS runtime cannot build here). hand-off notes:
the hand-off notes. Full per-ioctl table with sources: [`docs/reference/nvdrv-layouts.md`](../../reference/nvdrv-layouts.md).

## Why
The coverage items "/dev/nvhost-gpu, as-gpu, ctrl-gpu", "/dev/nvdisp-disp0", "/dev/nvhost-nvdec, vic, nvjpg, msenc" and "JPEG (nvjpg / caps)" were
partial because their layouts were written from memory and nothing pinned them. Every number, size and offset is now compared with a public source
(libnx, NVIDIA's L4T nvgpu / tegra_dc_ext / nvhost headers, NVIDIA open-gpu-doc class headers, Linux falcon.h), used as layout facts only.

## What changed
- `nvdrv_nodes_core.[ch]` (GPU nodes):
  - Fixed: as-gpu MAP_BUFFER_EX is nr 6 (was 4, which is not a Switch ioctl); INITIALIZE_EX reads flags at +0 and big_page_size at +8 (libnx order; was
    swapped), 0 = the 128K default; ctrl-gpu GET_GPU_TIME is nr 0x1C (was 0x14, which is ZBC_GET_ACTIVE_SLOT_MASK); GET_CHARACTERISTICS fields moved
    to the libnx offsets (flags +0x50, engine classes +0x58, chip name +0x90, warp count 0x80, VA bits 0x28, FBP/LTC/ROP fields); ZCULL_GET_INFO values as
    libnx reads them (width align 0x20); channel SET/GET_USER_DATA are type 'G' (new `nvgpu_chan_ioctl_typed`), 'H' 0x14 / 0x15 removed.
  - New: KICKOFF_PB, GET_ERROR_INFO, GET_ERROR_NOTIFICATION (libnx); ENABLE / DISABLE / PREEMPT / FORCE_RESET, EVENT_ID_CONTROL, ALLOC_GPFIFO_EX (L4T
    structs, Switch numbers by the "nr − 99" rule checked on the libnx rows); ZBC_GET_ACTIVE_SLOT_MASK; MAP_BUFFER_EX MODIFY flag and page-size write-back;
    SUBMIT FENCE_WAIT bookkeeping; size and number enums for every ioctl.
- `nvdrv_media_core.[ch]` (nvdisp): the invented ioctls (1 GET_MODE … 5 GET_STATUS) collided with tegra_dc_ext's numbers; the device now implements the
  tegra_dc_ext surface: SET_NVMAP_FD, GET/PUT_WINDOW, GET/PUT_CURSOR, SET_CURSOR_IMAGE, SET_CURSOR, SET_CSC, GET_STATUS / GET_VBLANK_SYNCPT (nr 9 by size),
  SET_LUT, CURSOR_CLIP, SET/GET CMU family, SET_VBLANK, GET/SET_WINMASK, GET_SCANLINE, all state-tracked.
- `nvdrv_media_regs.h` + `nvdrv_media_core.c` (engines):
  - NVDEC: display buffer luma/chroma at the real 0x10A/0x10B (were invented 0x130/0x131); histogram, scratch, external MV, H264_SET_MBHIST_BUF_OFFSET
    (MK8D's "unnamed 0x140"), VP8 and VP9 buffers named and known; the H.264 picture-info words MK8D's trace gave match `nvdec_h264_pic_s` exactly;
    VP9 strides read from word 25 (u16 pair; words 20/21 were the golden reference's size); VP8 stream length from VLDBufferSize (word 18; word 12 is
    inside the encryption block) and FrameStride; the status is the 14-word `nvdec_status_s` (width/height were written into the HEVC/VP9 statistics
    union; VP9/HEVC now report intra/inter block counts there).
  - VIC: methods 0x1C1–0x1CE confirmed by clceb6; slot input and config struct stay trace-matched (no public VIC 4.0 header).
  - NVJPG: rewritten on the real interface: cle7d0 methods 0x1C0–0x1C7, application id 1 decode / 2 encode, `nvjpg_drv_pic_param_s` (0x1240 bytes),
    `nvjpg_dec_status` / `nvjpg_enc_status`. Decode honours output type (YUV planar/semi-planar, YUY2, RGBA, BGRA, ABGR, ARGB), power-of-two downscale,
    chroma subsample, alpha and strides; encode produces the entropy-coded segment from the caller's quantiser factors and Huffman codes, as the hardware
    does. The invented 6/12-word setup is gone.
  - MSENC: clc5b7 methods (setup 0x1C4, status 0x1C6, bitstream 0x1C7, input 0x1CD/0x1D0/0x1D1; 0x101–0x104 were reference-picture registers),
    `nvenc_h264_drv_pic_setup_s` (input surface cfg, rate control, pic type), `nvenc_pic_stat_s`; planar input interleaved for the NV12 backend.
- `jpeg_core.[ch]`: `napjpeg_encode_scan` (scan-only baseline encode from planar / semi-planar YCbCr with caller tables, RST markers),
  `napjpeg_std_huff`, `napjpeg_huff_to_dht`; caller-given tables without a needed code now fail the encode instead of emitting garbage.
- Tests: `test_svc_nvdrv_nodes.c` (216 checks) and `test_svc_nvdrv_media.c` (293 checks) open with mirror structs (one source comment each) and
  `layout_pins()` (sizes, offsets, command words, method byte offsets), then round-trip every ioctl / method block through `nv_ioctl`. The media test's
  recording backend now copies the parameter sets it is given (it kept pointers into the engine's stack: an ASan stack-use-after-return that predates
  this change).
- Docs: `docs/reference/nvdrv-layouts.md` (new).

## Status
- Linux: `tools/linux-check/run-tests.sh` over all test binaries: the nvdrv, jpeg, caps and media tests pass; the remaining failures (18 mock-harness link
  failures, `test_svc_mii.c:35`, `test_svc_nifm_audit.c:54`) are identical on `main` and unrelated. The three touched tests are also clean under ASan +
  UBSan; `check-syntax.sh` clean (gcc, clang).
- macOS: **unbuilt** (commits marked `unbuilt:`). `nvdrv.c` was not touched; one routing line there is needed for the 'G' user-data ioctls on a GPU channel
  (patch in the hand-off notes).
- Not implemented on purpose (no reachable public layout; switchbrew could not be fetched): as-gpu REMAP, the ctrl-gpu debug / PMU / time-correlation
  ioctls, channel SET_TIMESLICE, nvdisp FLIP / FLIP4 / bandwidth / capture / CRC, NVJPG decode of a header-less scan. Listed per row in the layout table.
- No title of the four opens nvdisp, nvjpg or msenc, and the GPU nodes are only used when NVN is bypassed, so the X1-generation offsets of NVJPG / MSENC
  rest on NVIDIA's later class headers (method blocks that were stable across generations); a real stream would confirm them.
