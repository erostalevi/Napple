/* runtime/tests/host: what the host stand-ins expose to the tests. */
#pragma once
#include <stdint.h>
typedef struct { uint64_t heap, heap_size, free_lo, free_hi, last_dst; unsigned last_perm; int aliases; } HostCodeMemoryFake;
extern HostCodeMemoryFake host_code;
extern int host_fail_count;
extern char host_last_fail[512];
uint32_t host_thread_attach(void *tlr);
