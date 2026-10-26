/* romfs.map reader (development data directories built with napple build --update-romfs in-place). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef struct RomfsMap RomfsMap;
RomfsMap *romfs_map_open(const char *path, uint64_t *size); /* NULL when absent or malformed */
ssize_t romfs_map_pread(RomfsMap *m, void *dst, size_t len, uint64_t off);
void romfs_map_close(RomfsMap *m);
