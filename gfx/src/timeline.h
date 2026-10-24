/* timeline.h - submission timeline and deferred destruction (see timeline.c). */
#ifndef NAPPLE_GFX_TIMELINE_H
#define NAPPLE_GFX_TIMELINE_H
#include <stdint.h>
#include <stdatomic.h>
#ifdef __cplusplus
extern "C" {
#endif
uint64_t gfx_timeline_alloc(void);               /* next sequence number (1-based) */
uint64_t gfx_timeline_last(void);                /* last allocated */
void     gfx_timeline_complete(uint64_t seq);    /* GPU finished everything up to and including seq */
uint64_t gfx_completed(void);
int      gfx_wait_seq(uint64_t seq, uint64_t timeout_ns);                                   /* 1 when completed */
int      gfx_wait_cell(const _Atomic uint64_t *cell, uint64_t need, uint64_t timeout_ns);   /* 1 when *cell >= need */
void     gfx_wake(void);                         /* a cell changed */
void     gfx_retire(void (*fn)(void *), void *arg);  /* run fn(arg) once all work accepted so far has completed */
#ifdef __cplusplus
}
#endif
#endif
