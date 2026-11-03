/* swap.c: file-backed commit / decommit round trips, residency accounting and the trim passes. Portable (macOS and Linux). */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <unistd.h>
#include "swap.h"

int log_verbose;
void log_line(const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    setenv("NAPPLE_IOS_SWAP", "1", 1);
    CHECK(swap_enabled());
    const uint64_t cap = 256ULL << 20, page = 16384;
    uint8_t *region = mmap(NULL, cap, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(region != MAP_FAILED);
    SwapFile f;
    CHECK(swap_file_open(&f, "test", cap) == 0);

    /* commit grows in two steps, data written in the first step survives the second */
    CHECK(swap_commit(&f, region, 0, 32ULL << 20) != NULL);
    for (uint64_t o = 0; o < (32ULL << 20); o += page) region[o] = (uint8_t)(o / page + 1);
    CHECK(swap_commit(&f, region, 32ULL << 20, 32ULL << 20) != NULL);
    for (uint64_t o = 0; o < (32ULL << 20); o += page) CHECK(region[o] == (uint8_t)(o / page + 1));
    CHECK(region[40ULL << 20] == 0); /* fresh pages are zero */

    /* trims keep the data: write back, deactivate, drop everything */
    for (unsigned lvl = 0; lvl <= 2; lvl++) {
        swap_trim(lvl);
        for (uint64_t o = 0; o < (32ULL << 20); o += page) CHECK(region[o] == (uint8_t)(o / page + 1));
    }
    SwapStats s;
    swap_stats(&s);
    CHECK(s.files == 1 && s.committed == (64ULL << 20) && s.trims == 3);
    CHECK(s.resident > 0 && s.resident <= (64ULL << 20));

    /* decommit releases the range; mapping it again gives zeros, not the old data */
    CHECK(swap_decommit(&f, region, 0, 32ULL << 20) == 0);
    CHECK(swap_commit(&f, region, 0, 32ULL << 20) != NULL);
    int nonzero = 0;
    for (uint64_t o = 0; o < (32ULL << 20); o += page) nonzero += region[o] != 0;
#ifdef F_PUNCHHOLE
    CHECK(nonzero == 0);
#endif

    /* anywhere-mappings are file-backed, zeroed and writable */
    uint8_t *a = swap_map_anywhere(8ULL << 20);
    CHECK(a != NULL && a[0] == 0 && a[(8ULL << 20) - 1] == 0);
    if (a) { memset(a, 0x5a, 8ULL << 20); munmap(a, 8ULL << 20); }

    unsetenv("NAPPLE_IOS_SWAP");
    printf(fails ? "test_swap: %d FAILED\n" : "test_swap: ok\n", fails);
    return fails != 0;
}
