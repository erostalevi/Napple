/* A tiny RomFS image for the unit tests (public layout, see services/romfs_read.h):
 *   /a.txt "HELLO"            /dir/b.bin "BBBB"            /dir/sub/c.txt "CCC"
 * Returns the image size; `buf` must hold 0x400 bytes. */
#ifndef RT_ROMFS_FIXTURE_H
#define RT_ROMFS_FIXTURE_H
#include <stdint.h>
#include <string.h>

static size_t romfs_fixture_build(uint8_t *buf) {
    const uint32_t NONE = 0xffffffffu;
    memset(buf, 0, 0x400);
    const uint64_t dir_hash = 0x50, dir_hash_size = 16, dir_meta = 0x60, dir_meta_size = 80, file_hash = 0xb0, file_hash_size = 16, file_meta = 0xc0,
                   file_meta_size = 120, data = 0x140;
    uint64_t hdr[10] = {0x50, dir_hash, dir_hash_size, dir_meta, dir_meta_size, file_hash, file_hash_size, file_meta, file_meta_size, data};
    memcpy(buf, hdr, sizeof hdr);
    uint32_t *h = (uint32_t *)(buf + dir_hash);
    for (int i = 0; i < 4; i++) h[i] = NONE;
    h = (uint32_t *)(buf + file_hash);
    for (int i = 0; i < 4; i++) h[i] = NONE;
    /* directories: root (0), dir (24), sub (52); entry = parent, sibling, child dir, child file, hash next, name length, name (padded to 4) */
    uint32_t root[6] = {0, NONE, 24, 0, NONE, 0};
    uint32_t dir[6] = {0, NONE, 52, 40, NONE, 3};
    uint32_t sub[6] = {24, NONE, NONE, 80, NONE, 3};
    memcpy(buf + dir_meta, root, 24);
    memcpy(buf + dir_meta + 24, dir, 24);
    memcpy(buf + dir_meta + 24 + 24, "dir", 3);
    memcpy(buf + dir_meta + 52, sub, 24);
    memcpy(buf + dir_meta + 52 + 24, "sub", 3);
    /* files: a.txt (0), b.bin (40), c.txt (80); entry = parent, sibling, u64 data offset, u64 size, hash next, name length, name (padded to 4) */
    struct {
        uint32_t parent, sibling;
        uint64_t off, size;
        uint32_t hash, nlen;
    } fa = {0, NONE, 0, 5, NONE, 5}, fb = {24, NONE, 8, 4, NONE, 5}, fc = {52, NONE, 16, 3, NONE, 5};
    memcpy(buf + file_meta, &fa, 32);
    memcpy(buf + file_meta + 32, "a.txt", 5);
    memcpy(buf + file_meta + 40, &fb, 32);
    memcpy(buf + file_meta + 40 + 32, "b.bin", 5);
    memcpy(buf + file_meta + 80, &fc, 32);
    memcpy(buf + file_meta + 80 + 32, "c.txt", 5);
    memcpy(buf + data, "HELLO", 5);
    memcpy(buf + data + 8, "BBBB", 4);
    memcpy(buf + data + 16, "CCC", 3);
    return (size_t)data + 24;
}
/* A RomFS image with the single root file `name` holding `len` bytes of `data` (len < 0x1000, name < 24 characters); returns the image size, `buf` must hold 0x1100 bytes. */
static size_t romfs_fixture_one_file(uint8_t *buf, const char *name, const void *data, size_t len) {
    const uint32_t NONE = 0xffffffffu;
    memset(buf, 0, 0x1100);
    uint32_t nlen = (uint32_t)strlen(name), pad = (nlen + 3) & ~3u;
    uint64_t hdr[10] = {0x50, 0x50, 4, 0x58, 24, 0x70, 4, 0x78, 32 + pad, 0x100};
    memcpy(buf, hdr, sizeof hdr);
    memset(buf + 0x50, 0xff, 4);
    memset(buf + 0x70, 0xff, 4);
    uint32_t root[6] = {0, NONE, NONE, 0, NONE, 0};
    memcpy(buf + 0x58, root, 24);
    struct {
        uint32_t parent, sibling;
        uint64_t off, size;
        uint32_t hash, nlen;
    } f = {0, NONE, 0, len, NONE, nlen};
    memcpy(buf + 0x78, &f, 32);
    memcpy(buf + 0x78 + 32, name, nlen);
    memcpy(buf + 0x100, data, len);
    return 0x100 + len;
}
#endif
