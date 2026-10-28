/* Fuzz targets: romfs.map (romfs_map_open / romfs_map_pread) and the mod overlay (mods/overlay.json: romfs_overlay_load / lookup / child) of
 * services/romfs_map.c. Both are files in a data directory that comes from outside; the map also names the host files whose extents it serves. */
#include "../../src/services/romfs_map.h"
#include "fuzz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void write_file(const char *path, const void *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(p, 1, n, f);
    fclose(f);
}
enum { DATA_SIZE = 0x3000 };
static const char *data_path(void) { /* the host file the seed maps name: a few pages of known bytes */
    static char path[600];
    if (!path[0]) {
        uint8_t data[DATA_SIZE];
        for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 31 + (i >> 8));
        snprintf(path, sizeof path, "%s/romfs_data.bin", fuzz_workdir());
        write_file(path, data, sizeof data);
    }
    return path;
}

int fuzz_romfs_map(const uint8_t *d, size_t n) {
    char path[600];
    snprintf(path, sizeof path, "%s/romfs.map", fuzz_workdir());
    data_path();
    write_file(path, d, n);
    uint64_t size = 0;
    RomfsMap *m = romfs_map_open(path, &size);
    if (m) {
        uint8_t buf[0x2000];
        FzIn r = {d, n, n > 24 ? n - 24 : 0};
        uint64_t offs[] = {0, 1, size / 2, size ? size - 1 : 0, size, size + 1, 0xffffffffffffffffULL, fz_u32(&r), (uint64_t)fz_u32(&r) << 32 | fz_u32(&r)};
        for (unsigned i = 0; i < sizeof offs / sizeof *offs; i++) {
            romfs_map_pread(m, buf, sizeof buf, offs[i]);
            romfs_map_pread(m, buf, 1, offs[i]);
            romfs_map_pread(m, buf, 0, offs[i]);
        }
        for (uint64_t off = 0; off < size && off < 0x8000; off += 0x700) romfs_map_pread(m, buf, sizeof buf, off);
        romfs_map_close(m);
    }
    return 0;
}

void fuzz_romfs_map_seeds(FuzzSeedFn add, void *ctx) {
    const char *host = data_path();
    uint32_t hl = (uint32_t)strlen(host);
    /* header: RMP1, nfiles, nextents, size; then per file u32 length + path; then 64-byte extents {off, len, file_off, ctr, file, flags, hi[8], key[16]} */
    for (int variant = 0; variant < 3; variant++) {
        unsigned next = variant == 0 ? 1 : 3;
        uint8_t buf[24 + 4 + 600 + 64 * 3];
        size_t at = 0;
        memcpy(buf, "RMP1", 4);
        fz_put32(buf + 4, 1);
        fz_put64(buf + 8, next);
        fz_put64(buf + 16, variant == 0 ? DATA_SIZE : 0x2800);
        at = 24;
        fz_put32(buf + at, hl), at += 4;
        memcpy(buf + at, host, hl), at += hl;
        uint64_t lens[3] = {0x1000, 0x1000, 0x800}, off = 0;
        if (variant == 0) lens[0] = DATA_SIZE;
        for (unsigned i = 0; i < next; i++) {
            memset(buf + at, 0, 64);
            fz_put64(buf + at, off);
            fz_put64(buf + at + 8, lens[i]);
            fz_put64(buf + at + 16, variant == 2 ? 0x2000 - i * 0x1000 : off); /* file offset */
            fz_put64(buf + at + 24, 0x1234);
            at += 64;
            off += lens[i];
        }
        add(ctx, buf, at);
    }
}
FUZZ_TARGET(romfs_map);
