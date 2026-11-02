# nvdrv device layouts: ioctl numbers, structures, register maps and their sources

The GPU device nodes (`/dev/nvhost-gpu`, `-as-gpu`, `-ctrl-gpu`), the display head (`/dev/nvdisp-disp0/1`) and the host1x multimedia engines
(`/dev/nvhost-nvdec`, `-vic`, `-nvjpg`, `-msenc`) were written from memory up to 2026-11-01. On 2026-11-02 (branch
`claude/nvdrv-public-layouts-2026-11-02-r10`) every number, struct size and field offset was compared with public references and the mismatches were
fixed. This page is the record: one row per ioctl or method block, with the source that pins it and what the runtime does with it.

The references are used for layout facts only (numbers, sizes, offsets, field meaning). No driver code was copied.

| Key | Source | Licence | What it pins |
|---|---|---|---|
| **libnx** | [switchbrew/libnx](https://github.com/switchbrew/libnx) `nx/source/nvidia/ioctl/{nvchannel,nvhost-as-gpu,nvhost-ctrl-gpu}.c`, `nx/include/switch/nvidia/ioctl.h` (master, 2026-11) | ISC | The Switch numbering and struct sizes of the GPU nodes. Homebrew runs this on hardware, so the layouts are exercised. |
| **nvgpu** | NVIDIA L4T r32.6.1 nvgpu `include/uapi/linux/nvgpu.h` ([OE4T/linux-nvgpu](https://github.com/OE4T/linux-nvgpu) `l4t/l4t-r32.6.1`) | GPL-2.0 (header) | Field meaning, flag bits and constants of the GPU nodes. The Linux numbering differs from the Switch's, so it is used for numbers only where the "nr − 99" rule applies (below). |
| **dc_ext** | NVIDIA L4T `include/uapi/video/tegra_dc_ext.h` ([CTCaer/switch-l4t-kernel-nvidia](https://github.com/CTCaer/switch-l4t-kernel-nvidia) `linux-dev`, the Tegra X1 tree) | GPL-2.0 (header) | `/dev/nvdisp-disp*` ioctl numbers and LP64 struct sizes (computed by compiling the header in a scratch directory). |
| **ogd** | [NVIDIA/open-gpu-doc](https://github.com/NVIDIA/open-gpu-doc) `classes/video/`: `clc5b0.h` (NVDEC), `nvdec_drv.h`, `cle7d0.h` (NVJPG), `nvjpg_drv.h`, `clc9d1.h`, `clc5b7.h` (NVENC), `nvenc_drv.h`, `clceb6.h` (VIC) | MIT | Engine method offsets and the picture / status structures (offsets computed by compiling the headers). These are later engine generations than the X1's; see the notes per engine. |
| **falcon** | Linux `drivers/gpu/drm/tegra/falcon.h` (v6.6) | GPL-2.0 | The THI method FIFO: `FALCON_UCLASS_METHOD_OFFSET` 0x40 / `METHOD_DATA` 0x44 (words 0x10 / 0x11). |
| **l4t** | NVIDIA L4T `drivers/video/tegra/host/class_ids.h`, `host1x/host1x01_hardware.h` | GPL-2.0 | host1x class ids (NVDEC 0xF0, VIC 0x5D, NVJPG 0xC0, NVENC/MSENC 0x21) and the command-stream opcodes. |
| **trace** | MK8D's NVDEC + VIC submits (2026-10-27, `NAPPLE_MEDIA_TRACE`) | - | What a real title writes; used where the public headers are a later generation. |

switchbrew's NV services page could not be fetched from the build environment (DNS / proxy refused it), so it is not cited; where a number rests on
switchbrew alone it is listed as not implemented below.

**Status** values: *verified* = number, size and every field used match the source and a unit test pins them (size, offsets, command word, round trip);
*derived* = struct from the source, Switch number derived by a rule checked on neighbouring rows; *trace* = matched against a real title's stream, no public
header for that generation; *not implemented* = refused with NotImplemented on purpose (reason given).

## /dev/nvhost-gpu (channel; `nvdrv_nodes_core.c`)

| Type | Nr | Name | Size | Source | Status |
|---|---|---|---|---|---|
| H | 0x01 | SET_NVMAP_FD `{s32 fd}` | 4 | libnx | verified |
| H | 0x03 | SET_TIMEOUT `{u32 ms}` | 4 | libnx | verified |
| H | 0x08 | SUBMIT_GPFIFO `{u64 gpfifo, u32 num_entries, u32 flags, fence{u32 id, u32 value}}` + entries inline | 0x18 + 8n | libnx, nvgpu (flags: FENCE_WAIT 1, FENCE_GET 2) | verified |
| H | 0x09 | ALLOC_OBJ_CTX `{u32 class, u32 flags, u64 obj_id}` | 0x10 | libnx | verified |
| H | 0x0B | ZCULL_BIND `{u64 gpu_va, u32 mode, u32 pad}` | 0x10 | libnx | verified |
| H | 0x0C | SET_ERROR_NOTIFIER `{u64 offset, u64 size, u32 mem, u32 pad}` | 0x18 | libnx | verified |
| H | 0x0D | SET_PRIORITY `{u32 50/100/150}` | 4 | libnx | verified |
| H | 0x0E | ENABLE | 0 | nvgpu 113 | derived (new) |
| H | 0x0F | DISABLE (a disabled channel refuses submits) | 0 | nvgpu 114 | derived (new) |
| H | 0x10 | PREEMPT | 0 | nvgpu 115 | derived (new) |
| H | 0x11 | FORCE_RESET | 0 | nvgpu 116 | derived (new) |
| H | 0x12 | EVENT_ID_CONTROL `{u32 cmd, u32 event_id}` | 8 | nvgpu 117 (`nvgpu_event_id_ctrl_args` prefix) | derived (new) |
| H | 0x16 | GET_ERROR_INFO `{u32 type, u32 info[31]}` | 0x80 | libnx | verified (new) |
| H | 0x17 | GET_ERROR_NOTIFICATION `{u64 timestamp, u32 info32, u16 info16, u16 status}` | 0x10 | libnx | verified (new) |
| H | 0x18 | ALLOC_GPFIFO_EX `{u32 num_entries, u32 num_inflight_jobs, u32 flags, u32 reserved[5]}` | 0x20 | nvgpu 123 | derived (new) |
| H | 0x1A | ALLOC_GPFIFO_EX2 `{u32 num_entries, u32 flags, u32 unk0, fence out, u32 unk1..3}` | 0x20 | libnx | verified |
| H | 0x1B | KICKOFF_PB (SUBMIT_GPFIFO header; entries in the Ioctl2 buffer) | 0x18 | libnx | verified (new) |
| G | 0x14 | SET_USER_DATA `{u64}` (type 'G' on the channel fd) | 8 | libnx `_NV_IOW(0x47, 0x14)` | verified in the core; **needs the nvdrv.c routing patch** (integrator file) |
| G | 0x15 | GET_USER_DATA `{u64}` | 8 | libnx | as above |
| H | 0x14 / 0x15 | (were SET/GET_USER_DATA here) | - | - | removed: not channel ioctls of type 'H' |
| H | others (e.g. SET_TIMESLICE) | numbers documented only on switchbrew | - | switchbrew only | not implemented (no reachable public layout) |

The "nr − 99" rule: for every channel ioctl libnx confirms in that range, Switch nr = L4T nvgpu nr − 99 (SUBMIT_GPFIFO 107 → 0x08, ALLOC_OBJ_CTX 108 → 0x09,
ZCULL_BIND 110 → 0x0B, SET_ERROR_NOTIFIER 111 → 0x0C). It is applied to 113–117 and 123 only; it breaks after that (Linux 121 SET_TIMESLICE would land on
libnx's 0x16 GET_ERROR_INFO), so nothing past 0x18 uses it.

## /dev/nvhost-as-gpu (`nvdrv_nodes_core.c`)

| Type | Nr | Name | Size | Source | Status |
|---|---|---|---|---|---|
| A | 0x01 | BIND_CHANNEL `{s32 channel_fd}` | 4 | libnx | verified |
| A | 0x02 | ALLOC_SPACE `{u32 pages, u32 page_size, u32 flags, u32 pad, u64 offset/align}` (flags: FIXED 1, SPARSE 2) | 0x18 | libnx, nvgpu | verified |
| A | 0x03 | FREE_SPACE `{u64 offset, u32 pages, u32 page_size}` | 0x10 | libnx | verified |
| A | 0x04 | (was MAP_BUFFER_EX here) | - | - | **fixed**: not a Switch ioctl; NotImplemented |
| A | 0x05 | UNMAP_BUFFER `{u64 offset}` | 8 | libnx | verified |
| A | 0x06 | MAP_BUFFER_EX `{u32 flags, s32 kind, u32 nvmap_handle, u32 page_size (in/out), u64 buffer_offset, u64 mapping_size, u64 offset (in/out)}` (flags: FIXED 1, CACHEABLE 4, MODIFY 0x100) | 0x28 | libnx | verified (**fixed**: was nr 4; page size now written back; MODIFY new) |
| A | 0x08 | GET_VA_REGIONS `{u64 unused, u32 buf_size, u32 pad, region{u64 offset, u32 page_size, u32 pad, u64 pages}[2]}` | 0x40 | libnx, nvgpu | verified |
| A | 0x09 | INITIALIZE_EX `{u32 flags, s32 as_fd, u32 big_page_size, u32 reserved, u64 va_start, va_end, va_split}` | 0x28 | libnx (order), nvgpu (the three u64 read as the va range) | verified (**fixed**: big_page_size was read at +0 and flags at +8; 0 now means the 128K default GET_CHARACTERISTICS reports) |
| A | 0x14 | REMAP | variable | switchbrew only | not implemented (no reachable public layout) |

## /dev/nvhost-ctrl-gpu (`nvdrv_nodes_core.c`)

| Type | Nr | Name | Size | Source | Status |
|---|---|---|---|---|---|
| G | 0x01 | ZCULL_GET_CTX_SIZE `{u32}` | 4 | libnx | verified |
| G | 0x02 | ZCULL_GET_INFO (10 × u32) | 0x28 | libnx (values as read on hardware) | verified (**fixed**: width align 16 → 0x20) |
| G | 0x03 | ZBC_SET_TABLE `{u32 ds[4], l2[4], depth, format, type}` | 0x2C | libnx | verified |
| G | 0x04 | ZBC_QUERY_TABLE `{u32 ds[4], l2[4], depth, ref_cnt, format, type, index_size}` | 0x34 | libnx, nvgpu | verified |
| G | 0x05 | GET_CHARACTERISTICS `{u64 buf_size, u64 buf_addr, characteristics[0xA0]}` | 0xB0 | libnx (`nvioctl_gpu_characteristics`) | verified (**fixed**: flags were at +0x38 instead of +0x50, the class ids at +0x40 instead of +0x58, the chip name at +0x88 instead of +0x90, warp count 0x20 instead of 0x80; a short ioctl buffer is now BadParameter) |
| G | 0x06 | GET_TPC_MASKS `{u32 size, u32, u64 addr, u64 mask}` | 0x18 | libnx | verified |
| G | 0x07 | FLUSH_L2 `{u32 flush, u32 invalidate}` | 8 | libnx | verified |
| G | 0x14 | ZBC_GET_ACTIVE_SLOT_MASK `{u32 slot, u32 mask}` | 8 | libnx | verified (new; **fixed**: 0x14 was answered as GET_GPU_TIME) |
| G | 0x1C | GET_GPU_TIME `{u64 timestamp, u64 reserved}` | 0x10 | libnx, nvgpu | verified (**fixed**: was nr 0x14) |
| G | 0x0D–0x13, 0x15–0x1B, 0x1D | INVAL_ICACHE, debug modes, VSMS, PMU load, CPU time correlation, ... | - | switchbrew only (the Linux numbers do not carry over: GET_TPC_MASKS is Linux 10 but Switch 6) | not implemented (no reachable public layout) |

## /dev/nvdisp-disp0 / disp1 (`nvdrv_media_core.c`, type 'D')

The device used to answer invented ioctls (1 GET_MODE, 2 GET_VBLANK, 3 WAIT_VBLANK, 4 GET_SYNCPT, 5 GET_STATUS). They collided with the real numbers
(1 is GET_WINDOW, 2 PUT_WINDOW, 4 GET_CURSOR, 5 PUT_CURSOR) and are gone; the device now follows tegra_dc_ext.

| Nr | Name | Size | Source | Status |
|---|---|---|---|---|
| 0x00 | SET_NVMAP_FD `{s32}` | 4 | dc_ext | verified |
| 0x01 | GET_WINDOW `{u32 n}` (n < 3; held → InvalidState) | 4 | dc_ext | verified |
| 0x02 | PUT_WINDOW `{u32 n}` | 4 | dc_ext | verified |
| 0x03 | FLIP | - | removed from the public header | not implemented |
| 0x04 / 0x05 | GET_CURSOR / PUT_CURSOR | 0 | dc_ext | verified |
| 0x06 | SET_CURSOR_IMAGE `{u8 fg[3], bg[3], pad[2], u32 buff_id, u32 flags, s16 x, s16 y, u32 alpha, u32 colorfmt}` | 28 | dc_ext | verified |
| 0x07 | SET_CURSOR `{s16 x, s16 y, u32 flags}` | 8 | dc_ext | verified |
| 0x08 | SET_CSC `{u32 win_index, u16 yof, kyrgb, kur, kvr, kug, kvg, kub, kvb}` | 20 | dc_ext | verified |
| 0x09 | GET_STATUS `{u32 flags, u32 pad[3]}` | 16 | dc_ext | verified |
| 0x09 | GET_VBLANK_SYNCPT `{u32}` (same nr, told apart by size) | 4 | dc_ext | verified (syncpoint 26 / 27) |
| 0x0A | SET_LUT `{u32 win_index, flags, start, len, u64 r, g, b}` | 40 | dc_ext | verified (tables in guest memory are not read: no scanout) |
| 0x0C | CURSOR_CLIP `{s32}` | 4 | dc_ext | verified |
| 0x0D / 0x16 | SET_CMU / SET_CMU_ALIGNED `{u16 enable, csc[9], lut1[256], lut2[960]}` | 2452 | dc_ext | verified |
| 0x0F / 0x10 / 0x1C | GET_CMU / GET_CUSTOM_CMU / GET_CMU_ADBRGB | 2452 | dc_ext | verified |
| 0x15 | SET_VBLANK `{u8 enable, u8 reserved[3]}` | 4 | dc_ext | verified |
| 0x1E / 0x1F | GET_WINMASK / SET_WINMASK `{u32}` | 4 | dc_ext | verified |
| 0x24 | GET_SCANLINE `{u32}` (from the clock over the CEA vertical total: 750 / 1125) | 4 | dc_ext | verified |
| 0x13, 0x17–0x1B, 0x1D, 0x20–0x23, 0x25–0x28 | proposed bandwidth, nvdisp CMU/CSC (T186+), FLIP4, IMP, screen capture, SET_SCANLINE, CRC | - | dc_ext | not implemented (need a scanout engine) |

`/dev/nvdisp-ctrl` (type 'C', `tegra_dc_ext_control`) stays open-only in `nvdrv.c` (outside this change).

## Host1x channel ioctls (`nvdrv.c`, read-only check)

`nvdrv.c` was not edited (another session owns it). Its channel surface was compared with libnx and matches: `SUBMIT` (type 0, nr 1;
`{u32 cmdbufs, relocs, syncpt_incrs, fences}` + cmdbuf 12 B, reloc 16 B, shift 4 B, incr 20 B `{id, incrs, waitbase, next, prev}`, threshold 4 B),
`GET_SYNCPOINT` (2, 8 B), `GET_WAITBASE` (3, 8 B), `SET_SUBMIT_TIMEOUT` (7, 4 B), `MAP_CMD_BUFFER` / `UNMAP` (9 / 0xA, `{u32 num, u32 reserved, u8 compressed}` + 8 B
entries from +12), `SET_NVMAP_FD` / `SET_TIMEOUT` ('H' 1 / 3). Not answered there: `SET_CLK_RATE` (8) and `GET_CLK_RATE` (0x14 before 8.0.0, 0x23 after)
`{u32 rate, u32 module_id}` (libnx); they are listed for the nvdrv.c owner in the integrator file.

## Engine methods (`nvdrv_media_regs.h`)

All engines: SET_APPLICATION_ID word 0x80, EXECUTE 0xC0 (ogd, every class); THI method FIFO 0x10 / 0x11 (falcon); opcodes SETCL, INCR, NONINCR, MASK, IMM,
RESTART, GATHER (incl. the register form), EXTEND (l4t) - verified.

| Engine | Words | Methods | Source | Status |
|---|---|---|---|---|
| NVDEC 0xF0 | 0x100–0x10B | control params, pic setup, in buf, picture index, slice offsets, coloc, history, display buf size, histogram, status, display buf luma / chroma | ogd clc5b0 | verified (**fixed**: display buf luma/chroma were invented words 0x130/0x131, now 0x10A/0x10B; histogram 0x108 named) |
| NVDEC | 0x10C–0x12F | 17 picture luma, 17 chroma slots, pic scratch, external MV buffer | ogd clc5b0, trace | verified |
| NVDEC | 0x140, 0x150–0x151, 0x170–0x177 | H264_SET_MBHIST_BUF_OFFSET (MK8D's "unnamed 0x140"), VP8 prob / header partition, eight VP9 buffers | ogd clc5b0 | verified (named; known registers) |
| NVDEC pic | H.264 words 18–45 | stream_len 18, slice_count 19, POC/frame fields 22–24, size in MBs 25/26, tile 27, CABAC … 8x8 28–34, pitches 35/36, hist 43, flag words 44/45 (764 B) | ogd `nvdec_h264_pic_s`, trace | verified (the trace-derived words agree with the header exactly) |
| NVDEC pic | VP9 words 12, 24, 25 | stream_len; u16 width/height; u16 luma/chroma stride (256 B) | ogd `nvdec_vp9_pic_s` | verified (**fixed**: strides were read from words 20/21, which hold the golden reference's size) |
| NVDEC pic | VP8 words 14, 18, 19/20 | u16 width/height, VLDBufferSize (the frame size), FrameStride (0xC0 B) | ogd `nvdec_vp8_pic_s` | verified (**fixed**: stream length was read from word 12, inside the encryption block) |
| NVDEC status | 14 words | mbs correct, mbs in error, cycles, error_status, 9-word HEVC/VP9 statistics, slice header error (0x38 B) | ogd `nvdec_status_s` | verified (**fixed**: width/height were written into words 4/5, the statistics union; VP9/HEVC now get intra/inter block counts there) |
| VIC 0x5D | 0x1C1–0x1CE | control params, config struct, filter struct, output luma / chroma U / chroma V, status | ogd clceb6 | verified |
| VIC | 0x100 / 0x101 | slot 0 input luma / chroma | trace (VIC 4.0; clceb6 moved slots to 0x480) | trace |
| VIC config | +0x20 output surface u64 | format, block kind, block height, width − 1, height − 1 | trace + emulator layout (clceb6's VIC 4.2 config differs) | trace |
| NVJPG 0xC0 | 0x1C0–0x1C7 | control params, picture index, in drv pic setup, out status, bitstream, cur pic, chroma U, chroma V; app id 1 decode / 2 encode | ogd cle7d0 | verified (**fixed**: the engine used NVDEC's words 0x101–0x104/0x109 and an invented 6-word setup) |
| NVJPG pic | 0x1240 B | `nvjpg_drv_pic_param_s` with the encoder half: enc ctl / restart / quant factors (1<<15)/q / Huffman {length, value} at 0x000–0x6FF, PDMA 0x700, strides 0x708/0x70C, bitstream offset/size 0x720/0x724, decoder frame size/param 0x740/0x744, block ctl, Huffman mincode/symbol, output info 0x1120, quant 0x1140 | ogd `nvjpg_drv.h` | verified (new); decoder half **read for header-less decode** (2026-11-03, tested: round trips through the ioctl path, not traced from a title) |
| NVJPG decoder tables | in the pic | frame size (MCU grid) 0x740, frame param 0x744 (stream chroma mode, restart interval), block ctl[c] 0x748 (ac_idx b0-1, dc_idx b2-3, quant b4-5, hblock b8-11, vblock b12-15), quant `[i][t]` 0x1140, Huffman mincode `[2*slot+ac][len-1]` 0x760 and symbol `[slot][pos]` 0x8e0 | fields: ogd `nvjpg_drv.h`; **table semantics are Napple's reading** (below) | tested |
| NVJPG status | 0x20 B | decode `{bytes_offset, mcu_x, mcu_y, cycle_count, error_status, reserved[12]}`; encode `{bitstream_size, …}`; errors 0x30000001–4 | ogd `nvjpg_drv.h`, clc9d1 | verified (new) |
| MSENC 0x21 | 0x100–0x11F, 0x140–0x152, 0x1C0–0x1D9 | reference pictures, VP8/VP9 buffers, control params, picture index, RC data, in drv pic setup 0x1C4, out enc status 0x1C6, out bitstream 0x1C7, cur pic 0x1CD, chroma U/V 0x1D0/0x1D1 | ogd clc5b7 | verified (**fixed**: the engine read 0x101–0x104, which are IN_REF_PIC1..4_LUMA, and 0x109) |
| MSENC setup | 512 B | `nvenc_h264_drv_pic_setup_s`: magic, input surface cfg +0x24 (u16 width−1, height−1, pitch, chroma pitch; memory_mode bits 8–9), rate control +0x70 (framerate +0x18, nal_bitrate +0x24), pic_control +0xC8 (pic_type at +0xE0 bits 2–3) | ogd `nvenc_drv.h` | verified (new; replaces the invented 8-word setup) |
| MSENC status | 128 B | `nvenc_pic_stat_s`: picture index, error (2 bits + ucode error), total bits, pic type, slices, last valid byte | ogd `nvenc_drv.h` | verified (new) |

### Engine behaviour notes

- **NVJPG decode** reads the JPEG file the bitstream buffer holds (its SOI at or before `bitstream_offset`) with jpeg_core and honours output type
  (YUV planar / semi-planar, YUY2, RGBA, BGRA, ABGR, ARGB), power-of-two downscale, chroma subsample, alpha and strides. A buffer with only the
  entropy-coded data (no SOI in front) is the driver's header-less form and is decoded from the tables in the structure (next bullet); with no frame
  size programmed either, it is refused with BAD_MAGIC.
- **NVJPG header-less decode** (2026-11-03, `nvjpg_raw_params` in nvdrv_media_core.c, `napjpeg_decode_raw_rgba` in jpeg_core.c): the output surface is the
  one the headered path produces, byte for byte (both run the same MCU decoder). ogd pins the field positions; where it leaves the semantics open, Napple
  reads them as follows and the tests build the structure that way (nothing here was traced from a title; a real stream could differ in these points):
  picture size = `width_in_8x8 * 8 - width_pad` (and for the height) from the PDMA words, else the MCU grid; `dec_frame_size` = the MCU grid and must be
  `ceil(size / MCU size)`; `stream_chroma_mode` gives components and MCU size (0 mono, 1 4:2:0, 2 4:2:2H, 3 4:2:2V, 4 4:4:4); `restart_interval` is in MCUs;
  `dec_block_ctl[c]` gives the quantiser table, the Huffman slot (0..2) and the sampling factors (must agree with the chroma mode, 0/0 = the mode's);
  `dec_quant[i][t]` is table t's 8-bit factor i in zigzag order; Huffman slot s: `dec_huff_mincode[2s+ac][len-1] = {idx (first symbol index of that code
  length), val (first code)}`, `dec_huff_symbol[s][pos].val` the symbols, DC at pos 0..11 and AC at 12..173 (at most 12 DC / 162 AC symbols); the code
  counts are the idx steps (the last length's from the symbol list) and the mincode values are checked against the canonical codes of those counts; the
  `ac`/`comp`/`off` fields are not read. A slot without entries falls back to the component's own index. The entropy segment (RSTn kept) is at
  `bitstream_offset`, `bitstream_size` long (0: to the end of the buffer). Bad tables, an inconsistent geometry or a slot above 2: INVALID_INPUT.
- **NVJPG encode** produces what the hardware does: the entropy-coded segment only (with RSTn markers), from the caller's quantiser factors and Huffman
  codes, written at `bitstream_offset`. The 176-entry AC tables are read as 16 runs × 11 sizes (index = run × 11 + size; 162 entries used, matching the
  header's comment). Width/height come from the PDMA 8×8 counts minus the pad fields; memory_mode 0 = semi-planar, 1 = planar (the convention
  `nvenc_drv.h` documents for the same field). The unit test writes the driver's headers in front of the output and decodes it with jpeg_core.
- **MSENC** hands one NV12 frame per execute to the EncoderBackend; planar input is interleaved first. Only the fields listed above are read.
- Generation caveat: open-gpu-doc carries Turing/Ampere-era class headers. The X1's engines are earlier (NVDEC 2, VIC 4.0, NVJPG 1, NVENC 5). Method
  blocks have been stable across these generations (the NVDEC words match MK8D's trace exactly), but a real NVJPG/MSENC stream from a title would confirm
  the X1 offsets; none of the four test titles opens those two devices.

## Tests

`runtime/tests/test_svc_nvdrv_nodes.c` and `runtime/tests/test_svc_nvdrv_media.c` start with mirror structs (one source comment each) and `layout_pins()`:
`sizeof` and `offsetof` of every structure above against the constants the code uses, the ioctl command words libnx / tegra_dc_ext encode, the method
byte offsets of the class headers; then each ioctl / method block is round-tripped through the real `nv_ioctl` handler.

## Round 11 additions (claude/nvdrv-gpu-routing-2026-11-03-r11, integrated onto the round-10 table)
The cloud session had no network access to switchbrew or the NVIDIA headers and did not see this table: its layouts are written from memory and are **proposed**, never checked against a source or trace. Status *tested* = unit test (`runtime/tests/test_svc_nvdrv_nodes.c`).
Rows kept next to the round-10 ones (the round-10 numbers and layouts above stay authoritative; the branch's other rows duplicated them: its 0x17 GET_ERROR_NOTIFICATION is the round-10 row):

| type | nr | name | struct | status |
|---|---|---|---|---|
| 'H' | 0x1D | EVENT_ID_CTRL (branch form, shares the enable mask of 0x12) | {u32 cmd 0 disable/1 enable/2 clear, u32 event_id 0..2, s32 fd, u32} (0x10); clear keeps the mask, the fd is not kept | tested, proposed |
| 'G' | 0x0C | GET_TPC_EXCEPTION_EN_STATUS | {u64 mask out} (8): 0, no SM exception enabled | tested, proposed |
| 'G' | 0x0D | NUM_VSMS | {u32 num out = 2, u32 reserved = 0} (8) | tested, proposed |

The branch's list of ioctls "that stay NotImplemented" (KICKOFF_PB, SET_TIMESLICE, REMAP, ...) is superseded by the round-10 table above. Routing: unchanged (`gpu_node_ioctl` in `nvdrv.c`).
