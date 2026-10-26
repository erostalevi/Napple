/* Force-included: Darwin names movie.c uses. */
#pragma once
#include <stdint.h>
#include <time.h>
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
static inline uint64_t clock_gettime_nsec_np(int clk) { struct timespec t; clock_gettime(clk, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
