/* Linux stand-in for the few Mach declarations the portable runtime sources mention (debug paths only), force-included by tests/linux/run.sh. */
#pragma once
#include <pthread.h>
#include <stdint.h>
typedef unsigned mach_port_t;
typedef unsigned mach_msg_type_number_t;
typedef struct { uint64_t __x[29], fp, lr, sp, pc; uint32_t cpsr; } arm_thread_state64_t;
#define ARM_THREAD_STATE64 6
#define ARM_THREAD_STATE64_COUNT 68
#define KERN_SUCCESS 0
typedef void *thread_state_t;
static inline mach_port_t pthread_mach_thread_np(pthread_t t) { (void)t; return 0; }
static inline int thread_get_state(mach_port_t t, int f, void *s, mach_msg_type_number_t *n) { (void)t; (void)f; (void)s; (void)n; return 1; }
static inline uint64_t arm_thread_state64_get_pc(arm_thread_state64_t s) { return s.pc; }
static inline uint64_t arm_thread_state64_get_fp(arm_thread_state64_t s) { return s.fp; }
static inline uint64_t arm_thread_state64_get_lr(arm_thread_state64_t s) { return s.lr; }
static inline int thread_suspend(mach_port_t t) { (void)t; return 1; }
static inline int thread_resume(mach_port_t t) { (void)t; return 1; }
