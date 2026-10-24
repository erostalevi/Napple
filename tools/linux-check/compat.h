/* Force-included (-include) by the Linux checks: BSD/Darwin functions the services use that glibc lacks. */
#pragma once
#include <stddef.h>
#include <stdint.h>
size_t strlcpy(char *dst, const char *src, size_t size);
void arc4random_buf(void *buf, size_t n);
