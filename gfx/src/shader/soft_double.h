/* soft_double.h - host (C) build of the binary64 emulation used by the Maxwell translator for DADD/DMUL/DFMA/DSETP/DMNMX/DSET and the 64-bit
 * conversions. The logic is soft_double_core.inc, which the translator also emits, token for token, into Metal shaders (soft_double_msl.h).
 * Used by gfx/tests/t_softdouble.c (Linux and Mac) to verify the algorithm against the CPU's native double.
 * Raw bit patterns are used throughout: sd_u64 is the binary64, sd_u32 a binary32. Rounding: 0 RN, 1 RM, 2 RP, 3 RZ. */
#ifndef SOFT_DOUBLE_H
#define SOFT_DOUBLE_H
#include <stdint.h>

typedef uint32_t sd_u32;
typedef uint64_t sd_u64;

#define uint sd_u32
#define ulong sd_u64
#define SD_FN static inline
#define SD_CODE(...) __VA_ARGS__
static inline sd_u32 sd_clz32_host(sd_u32 x) { return x ? (sd_u32)__builtin_clz(x) : 32u; }
#define clz sd_clz32_host
#include "soft_double_core.inc"
#undef clz
#undef SD_CODE
#undef SD_FN
#undef ulong
#undef uint
#undef SD_SIGN
#undef SD_MAG
#undef SD_FRAC
#undef SD_IMPL
#undef SD_INF
#undef SD_NAN
#undef SD_ONE

#endif
