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
    uint64_t words, ops, regwrites, executes, frames, errors, unknown_regs, foreign_writes, syncpt_incrs;
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
