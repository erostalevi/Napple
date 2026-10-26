#ifndef NAPPLE_INPUT_CHECK_H
#define NAPPLE_INPUT_CHECK_H
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int g_checked, g_failed;
#define CHECK(cond, ...)                                                                                                                                       \
    do {                                                                                                                                                       \
        g_checked++;                                                                                                                                           \
        if (!(cond)) {                                                                                                                                         \
            g_failed++;                                                                                                                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                                                               \
            fprintf(stderr, __VA_ARGS__);                                                                                                                      \
            fprintf(stderr, "\n");                                                                                                                             \
        }                                                                                                                                                      \
    } while (0)
static inline int nearf(float a, float b, float eps) { return fabsf(a - b) <= eps; }
#endif
