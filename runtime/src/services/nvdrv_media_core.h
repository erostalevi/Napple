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
    uint64_t words, ops, regwrites, executes, frames, errors, unknown_regs, foreign_writes, syncpt_incrs, configures;
    uint32_t last_unknown_reg, last_error;
} MediaStats;
typedef struct MediaEngine MediaEngine;

/* NULL for a class that is not a media engine. The backend is the installed one at the time of the call. */
MediaEngine *media_engine_new(uint32_t class_id, const NvMediaMem *mem);
void media_engine_free(MediaEngine *e);
/* Runs one command buffer. MEDIA_OK, MEDIA_BAD_STREAM (unknown opcode, truncated op) or MEDIA_BAD_ADDRESS (a gather that is not mapped). A failed decode is
 * not an error here: it is counted, reported in the status structure the guest asked for, and the stream continues. */
int media_engine_run(MediaEngine *e, const uint32_t *words, uint32_t n);
const MediaStats *media_engine_stats(const MediaEngine *e);
uint32_t media_engine_reg(const MediaEngine *e, uint32_t offset); /* last value written to a register of the engine class (tests, traces) */
uint32_t media_engine_class(const MediaEngine *e);

/* ---- /dev/nvdisp-disp0 (portable) ----------------------------------------------------------------------------------------------------
 * The display controller's query surface. Napple presents through the host (vi/NVN), so there is no scanout hardware to drive; the device answers what a
 * guest asks of a display: its mode, the vblank counter and the vblank syncpoint. The ioctl numbers and structs below are PROPOSED (the public driver sources
 * do not document a stable nvdisp ABI; nothing here is verified against real traces). All ioctls use type NVDISP_IOCTL_TYPE ('D'); sizes are the struct sizes.
 *   nr 1 GET_MODE     out: u32 width, height, refresh_mhz (milli-Hz), bpp; u32 flags (bit0 docked); u32 pad[3]          (32 bytes)
 *   nr 2 GET_VBLANK   out: u64 count, u64 timestamp_ns                                                                   (16 bytes)
 *   nr 3 WAIT_VBLANK  in: u32 timeout_ms, u32 pad; out: u64 count at +8, u64 timestamp_ns at +16. Does not block: it reports the next vblank (count is strictly
 *                     increasing per call even when the host clock has not moved a frame)                                (24 bytes)
 *   nr 4 GET_SYNCPT   out: u32 syncpt id, u32 current value                                                              (8 bytes; id set by nvdisp_set_syncpt)
 *   nr 5 GET_STATUS   out: u32 connected, powered, docked, vblank_enabled                                                (16 bytes)
 * Anything else is NotImplemented (the caller logs it in the compat summary). Time is passed in so the tests are deterministic. */
enum { NVDISP_IOCTL_TYPE = 'D', NVDISP_GET_MODE = 1, NVDISP_GET_VBLANK = 2, NVDISP_WAIT_VBLANK = 3, NVDISP_GET_SYNCPT = 4, NVDISP_GET_STATUS = 5 };
enum { NVDISP_OK = 0, NVDISP_NOT_IMPLEMENTED = 1, NVDISP_BAD_PARAMETER = 4 };
typedef struct {
    uint32_t width, height, refresh_mhz, docked;
    uint32_t syncpt;
    uint64_t t0_ns, last_wait;
} NvDisp;
/* Handheld 1280x720 at 60 Hz unless docked (1920x1080). t0 is the time the counter starts at 0. */
void nvdisp_init(NvDisp *d, uint64_t now_ns, int docked);
uint64_t nvdisp_vblank_count(const NvDisp *d, uint64_t now_ns);
/* data is the in/out buffer (size bytes, at least the struct size of the ioctl). Returns an NVDISP_* code. */
int nvdisp_ioctl(NvDisp *d, unsigned nr, uint8_t *data, unsigned size, uint64_t now_ns);
