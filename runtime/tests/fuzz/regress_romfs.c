/* Regression tests for the crashes the romfs_map fuzz target found (FINDINGS.md). Build with the sanitizers: make -C runtime fuzz-regress. */
#include "../../src/services/romfs_map.h"
#include "../check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* An allocation sized from the file header must not exceed what the file can hold. */
const char *__asan_default_options(void) { return "detect_leaks=0:max_allocation_size_mb=256"; }

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static RomfsMap *open_bytes(const uint8_t *b, size_t n, uint64_t *size) {
    const char *path = "regress_romfs.tmp";
    FILE *f = fopen(path, "wb");
    fwrite(b, 1, n, f);
    fclose(f);
    RomfsMap *m = romfs_map_open(path, size);
    remove(path);
    return m;
}

int main(void) {
    uint64_t size = 0;
    uint8_t h[24];
    memset(h, 0, sizeof h);
    memcpy(h, "RMP1", 4);

    /* 1. More than 64 files: romfs_map_open refused the header before allocating the descriptor table, and romfs_map_close then walked
     * m->fds[0..nfiles) through a NULL pointer (UBSan: applying zero offset to null pointer, then a crash). Found by fuzz romfs_map, seed 1, iteration 8. */
    put32(h + 4, 100), put64(h + 8, 1), put64(h + 16, 4096);
    CHECK(open_bytes(h, sizeof h, &size) == NULL, "100 files is refused without a crash");

    /* 2. An extent count the file cannot hold (up to 2^26 records of 80 bytes were allocated before the first record was read). */
    put32(h + 4, 1), put64(h + 8, 1u << 26), put64(h + 16, 4096);
    uint8_t big[24 + 4 + 8 + 64];
    memcpy(big, h, 24);
    put32(big + 24, 8);
    memcpy(big + 28, "/dev/nul", 8);
    memset(big + 36, 0, 64);
    CHECK(open_bytes(big, sizeof big, &size) == NULL, "2^26 extents in a 100 byte file is refused");

    /* the valid shape still opens and reads: one file, one extent covering it */
    const char *data = "regress_romfs.dat";
    FILE *f = fopen(data, "wb");
    fwrite("0123456789abcdef", 1, 16, f);
    fclose(f);
    uint8_t ok[24 + 4 + 17 + 64];
    memset(ok, 0, sizeof ok);
    memcpy(ok, "RMP1", 4);
    put32(ok + 4, 1), put64(ok + 8, 1), put64(ok + 16, 16);
    put32(ok + 24, 17);
    memcpy(ok + 28, data, 17);
    put64(ok + 45, 0), put64(ok + 45 + 8, 16), put64(ok + 45 + 16, 0); /* off 0, len 16, file_off 0 */
    RomfsMap *m = open_bytes(ok, sizeof ok, &size);
    CHECK(m != NULL && size == 16, "a valid map opens");
    char buf[8] = {0};
    CHECK(m && romfs_map_pread(m, buf, 4, 10) == 4 && !memcmp(buf, "abcd", 4), "and reads its extent");
    CHECK(m && romfs_map_pread(m, buf, 8, 12) == 4, "a read is cut at the end of the extent");
    CHECK(m && romfs_map_pread(m, buf, 8, 16) == -1, "a read at the end fails");
    romfs_map_close(m);
    remove(data);
    printf("{\"test\":\"regress_romfs\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
