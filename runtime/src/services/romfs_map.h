/* romfs.map reader (development data directories built with napple build --update-romfs in-place). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef struct RomfsMap RomfsMap;
RomfsMap *romfs_map_open(const char *path, uint64_t *size); /* NULL when absent or malformed */
ssize_t romfs_map_pread(RomfsMap *m, void *dst, size_t len, uint64_t off);
void romfs_map_close(RomfsMap *m);

/* Mod overlay (napple build --mods): <data-dir>/mods/overlay.json maps romfs paths to host files, or hides them.
 * One entry per line: {"path":"/a/b","file":"mods/files/0/a/b","size":N} or {"delete":"/a/b"} (a delete hides the subtree).
 * Paths are absolute romfs paths; files are relative to the data directory. */
typedef struct RomfsOverlay RomfsOverlay;
enum { ROMFS_OVERLAY_ORIGINAL = 0, ROMFS_OVERLAY_REPLACED = 1, ROMFS_OVERLAY_DELETED = -1 };
RomfsOverlay *romfs_overlay_load(const char *data_dir); /* NULL when absent, empty or malformed */
/* Resolves one romfs path. REPLACED fills host (absolute host path) and size; DELETED and ORIGINAL leave them alone. */
int romfs_overlay_lookup(const RomfsOverlay *o, const char *path, char *host, size_t cap, uint64_t *size);
/* Directory listing of dir_path: nth overlay entry that is a direct child (name copied, is_dir set when the child is only an
 * intermediate directory of a deeper replacement); returns 0 when n is past the end. Deleted names of the original are the
 * caller's to filter through romfs_overlay_lookup. */
int romfs_overlay_child(const RomfsOverlay *o, const char *dir_path, size_t n, char *name, size_t cap, int *is_dir);
void romfs_overlay_free(RomfsOverlay *o);
