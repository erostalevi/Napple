/* Minimal test harness for the runtime tests (same shape as input/tests/check.h). */
#ifndef RT_CHECK_H
#define RT_CHECK_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int g_checked, g_failed;
#define CHECK(cond, ...) do { g_checked++; if (!(cond)) { g_failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#endif
