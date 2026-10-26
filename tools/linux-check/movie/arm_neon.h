/* Stub of the few NEON names runtime/src/movie.c uses (syntax check only: values are never computed). */
#pragma once
#include <stdint.h>
typedef struct { uint8_t v[16]; } uint8x16_t;
typedef struct { uint8x16_t val[2]; } uint8x16x2_t;
static inline uint8x16_t vld1q_u8(const uint8_t *p) { uint8x16_t r; __builtin_memcpy(r.v, p, 16); return r; }
static inline void vst2q_u8(uint8_t *p, uint8x16x2_t x) { (void)p; (void)x; }
