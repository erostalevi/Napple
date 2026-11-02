/* RomFS image reader (portable): walks the directory and file tables of a RomFS image (the decrypted IVFC data level, as fsp-srv serves it as an
 * IStorage) so a file system view can be answered without the guest parsing the tables. Layout (public, libnx/switchbrew): header {u64 size,
 * dir hash off/size, dir meta off/size, file hash off/size, file meta off/size, u64 file data off}; directory entry {u32 parent, sibling, child dir,
 * child file, hash next, name length, name}; file entry {u32 parent, sibling, u64 data offset, u64 data size, u32 hash next, name length, name}.
 * The image is read through a callback (romfs_map.h RomfsReadFn), so any backend of fs.c works: romfs.bin, romfs.map, delta layer, mod overlay. */
#pragma once
#include "romfs_map.h"

typedef struct RomfsImg RomfsImg;
enum { ROMFS_NONE = 0, ROMFS_FILE = 1, ROMFS_DIR = 2 };

/* Parses the tables; NULL when the image is not a RomFS. The tables are copied, `rd` is only used for reading file data afterwards. */
RomfsImg *romfs_img_open(RomfsReadFn rd, void *ctx, uint64_t size);
void romfs_img_close(RomfsImg *img);
/* "/a/b", "a/b" or "" for the root. ROMFS_FILE fills data offset (relative to the file data area) and size; ROMFS_DIR leaves them 0; ROMFS_NONE when absent. */
int romfs_img_lookup(const RomfsImg *img, const char *path, uint64_t *off, uint64_t *size);
/* The n-th child of directory `dir` (directories first, then files, in table order); 0 when `dir` is not a directory or n is past the end. */
int romfs_img_child(const RomfsImg *img, const char *dir, unsigned n, char *name, size_t cap, int *is_dir, uint64_t *size);
/* Reads `len` bytes of a file at (data offset `off`, size `fsize`) from position `pos`; bytes read, 0 at the end, -1 on error. */
ssize_t romfs_img_read(const RomfsImg *img, uint64_t off, uint64_t fsize, uint64_t pos, void *dst, size_t len);
