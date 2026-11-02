/* nvdrv media core: the host1x command-stream parser and the register-level engines behind /dev/nvhost-nvdec, -vic, -nvjpg and -msenc (portable;
 * nvdrv.c supplies the memory resolver and calls media_engine_run for every command buffer of a SUBMIT). Opcodes follow the host1x stream
 * format of the public driver sources (SETCL, INCR, NONINCR, MASK, IMM, RESTART, GATHER, EXTEND). The engines are documented in
 * docs/architecture/runtime-services.md; register offsets are in nvdrv_media_regs.h (unverified against real traces). */
#pragma once
#include "decoder_backend.h"
#include <stddef.h>
#include <stdint.h>

enum { MEDIA_OK = 0, MEDIA_BAD_STREAM = 1, MEDIA_BAD_ADDRESS = 2 };
enum { MEDIA_CLASS_NVDEC = 0xF0, MEDIA_CLASS_VIC = 0x5D, MEDIA_CLASS_NVJPG = 0xC0, MEDIA_CLASS_MSENC = 0x21 };

typedef struct {
    /* Host pointer for the device address `iova` and how many bytes follow it inside the same mapping; NULL when not mapped. */
    uint8_t *(*resolve)(void *ctx, uint64_t iova, size_t *avail);
    void *ctx;
} NvMediaMem;
typedef struct {
    uint64_t words, ops, regwrites, executes, frames, errors, unknown_regs, foreign_writes, syncpt_incrs, configures, thi_pairs, vic_copies;
    uint32_t last_unknown_reg, last_error;
} MediaStats;
typedef struct MediaEngine MediaEngine;

/* NULL for a class that is not a media engine. The backend is the installed one at the time of the call. */
MediaEngine *media_engine_new(uint32_t class_id, const NvMediaMem *mem);
void media_engine_free(MediaEngine *e);
/* Runs one command buffer. MEDIA_OK, MEDIA_BAD_STREAM (unknown opcode, truncated op) or MEDIA_BAD_ADDRESS (a gather that is not mapped). A failed decode is
 * not an error here: it is counted, reported in the status structure the guest asked for, and the stream continues. */
int media_engine_run(MediaEngine *e, const uint32_t *words, uint32_t n);
/* Forgets the surface pitches NVDEC recorded for VIC (tests). */
void media_pitch_hints_reset(void);
const MediaStats *media_engine_stats(const MediaEngine *e);
uint32_t media_engine_reg(const MediaEngine *e, uint32_t offset); /* last value written to a register of the engine class (tests, traces) */
uint32_t media_engine_class(const MediaEngine *e);

/* ---- /dev/nvdisp-disp0 (portable) ----------------------------------------------------------------------------------------------------
 * The display head's tegra_dc_ext surface. Napple presents through the host (vi/NVN), so no scanout hardware is driven; the device keeps the state a
 * client can set and observe (window / cursor ownership, cursor, CSC, LUT, CMU, window mask, vblank enable) and answers the queries (status, vblank
 * syncpoint, scanline) from the mode and the clock. Layout source: NVIDIA's public tegra_dc_ext UAPI (include/uapi/video/tegra_dc_ext.h of the L4T kernel,
 * the CTCaer/switch-l4t-kernel-nvidia tree), layout facts only. Every ioctl is type 'D' (0x44); the size is the struct size of the LP64 header:
 *   0x00 SET_NVMAP_FD{s32}(4)   0x01 GET_WINDOW{u32 n}(4)   0x02 PUT_WINDOW{u32 n}(4)   0x04 GET_CURSOR(0)   0x05 PUT_CURSOR(0)
 *   0x06 SET_CURSOR_IMAGE{u8 fg[3],u8 bg[3],pad[2],u32 buff_id,u32 flags,s16 x,s16 y,u32 alpha,u32 colorfmt}(28)   0x07 SET_CURSOR{s16 x,s16 y,u32 flags}(8)
 *   0x08 SET_CSC{u32 win_index,u16 yof,kyrgb,kur,kvr,kug,kvg,kub,kvb}(20)
 *   0x09 GET_STATUS{u32 flags,u32 pad[3]}(16) and GET_VBLANK_SYNCPT{u32}(4): the same nr, told apart by the size
 *   0x0A SET_LUT{u32 win_index,flags,start,len,u64 r,g,b}(40)   0x0C CURSOR_CLIP{s32}(4)
 *   0x0D SET_CMU / 0x0F GET_CMU / 0x10 GET_CUSTOM_CMU / 0x16 SET_CMU_ALIGNED / 0x1C GET_CMU_ADBRGB {u16 enable,u16 csc[9],u16 lut1[256],u16 lut2[960]}(2452)
 *   0x15 SET_VBLANK{u8 enable,u8 reserved[3]}(4)   0x1E GET_WINMASK{u32}(4)   0x1F SET_WINMASK{u32}(4)   0x24 GET_SCANLINE{u32}(4)
 * FLIP (0x03, removed from the public header), the FLIP4 / proposed-bandwidth / IMP / screen-capture / CRC families and SET_SCANLINE are NotImplemented:
 * they need a scanout engine or a layout this header does not pin for the Switch (docs/reference/nvdrv-layouts.md). A head has three windows (A, B, C).
 * Time is passed in so the tests are deterministic. */
enum { NVDISP_IOCTL_TYPE = 'D' };
enum {
    NVDISP_SET_NVMAP_FD = 0x00, NVDISP_GET_WINDOW = 0x01, NVDISP_PUT_WINDOW = 0x02, NVDISP_GET_CURSOR = 0x04, NVDISP_PUT_CURSOR = 0x05,
    NVDISP_SET_CURSOR_IMAGE = 0x06, NVDISP_SET_CURSOR = 0x07, NVDISP_SET_CSC = 0x08, NVDISP_GET_STATUS = 0x09, NVDISP_GET_VBLANK_SYNCPT = 0x09,
    NVDISP_SET_LUT = 0x0a, NVDISP_CURSOR_CLIP = 0x0c, NVDISP_SET_CMU = 0x0d, NVDISP_GET_CMU = 0x0f, NVDISP_GET_CUSTOM_CMU = 0x10, NVDISP_SET_VBLANK = 0x15,
    NVDISP_SET_CMU_ALIGNED = 0x16, NVDISP_GET_CMU_ADBRGB = 0x1c, NVDISP_GET_WINMASK = 0x1e, NVDISP_SET_WINMASK = 0x1f, NVDISP_GET_SCANLINE = 0x24
};
enum {
    NVDISP_SZ_S32 = 4, NVDISP_SZ_CURSOR_IMAGE = 28, NVDISP_SZ_CURSOR = 8, NVDISP_SZ_CSC = 20, NVDISP_SZ_STATUS = 16, NVDISP_SZ_LUT = 40, NVDISP_SZ_CMU = 2452,
    NVDISP_SZ_SET_VBLANK = 4
};
enum { NVDISP_WINDOWS = 3, NVDISP_STATUS_ENABLED = 1, NVDISP_CURSOR_VISIBLE = 1 };
enum { NVDISP_OK = 0, NVDISP_NOT_IMPLEMENTED = 1, NVDISP_BAD_PARAMETER = 4, NVDISP_INVALID_STATE = 8 /* NvError values (nvdrv.c passes them through) */ };
typedef struct {
    uint16_t enable, csc[9], lut1[256], lut2[960];
} NvDispCmu; /* tegra_dc_ext_cmu, 2452 bytes */
typedef struct {
    uint32_t width, height, refresh_mhz, docked;
    uint32_t syncpt;   /* vblank syncpoint id (nvdrv.c sets it: 26 for disp0, 27 for disp1) */
    uint64_t t0_ns;
    int32_t nvmap_fd, cursor_clip;
    uint32_t windows;  /* GET_WINDOW ownership mask (bit per window) */
    uint32_t winmask;  /* SET_WINMASK: windows assigned to this head */
    int cursor_owned, vblank_enabled, cmu_custom;
    int16_t cursor_x, cursor_y;
    uint32_t cursor_flags, cursor_buff_id, cursor_image_flags, cursor_alpha, cursor_colorfmt;
    uint8_t cursor_fg[3], cursor_bg[3];
    uint16_t csc[NVDISP_WINDOWS][8]; /* yof, kyrgb, kur, kvr, kug, kvg, kub, kvb */
    uint32_t lut_flags[NVDISP_WINDOWS], lut_start[NVDISP_WINDOWS], lut_len[NVDISP_WINDOWS];
    NvDispCmu cmu;
} NvDisp;
/* Handheld 1280x720 at 60 Hz unless docked (1920x1080). t0 is the time the vblank counter starts at 0. Window A is assigned to the head. */
void nvdisp_init(NvDisp *d, uint64_t now_ns, int docked);
uint64_t nvdisp_vblank_count(const NvDisp *d, uint64_t now_ns);
/* data is the in/out buffer (size bytes, at least the struct size of the ioctl). Returns an NVDISP_* code. */
int nvdisp_ioctl(NvDisp *d, unsigned nr, uint8_t *data, unsigned size, uint64_t now_ns);

/* Optional: called before the engines write decoded pixels into a guest surface (gfx dirty tracking). Set by media_gfx_glue.c in the runtime, NULL in host tests. */
extern void (*media_write_prepare)(void *addr, size_t n);
/* NAPPLE_MEDIA_TRACE=1: debug prints of the media path (decodes, VIC jobs, nvmap allocations and pins, unresolved device addresses) to stderr. */
int media_trace(void);
