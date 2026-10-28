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

/* Merged RomFS (path-level mod overlay). fs.c serves the RomFS as a raw IStorage image and the guest parses the directory and file tables
 * itself, so the overlay cannot be a lookup: this builds a VIRTUAL RomFS image. The original image is read through `rd` (any backend:
 * romfs.bin, romfs.map, delta layer), its tables are parsed into a tree, the overlay is applied (deletes first, then replacements/additions;
 * a replacement under a deleted subtree wins, as in romfs_overlay_lookup), and fresh header, directory/file tables and hash tables are
 * generated in memory. File data is not copied: every file is an extent of the original image or of a host file (fds opened on demand).
 * Returns NULL (reason in why) when the original is not a parsable RomFS; the caller then serves the original image. `o` is not kept. */
typedef ssize_t (*RomfsReadFn)(void *ctx, void *dst, size_t len, uint64_t off);
typedef struct RomfsMerged RomfsMerged;
RomfsMerged *romfs_merge_open(const RomfsOverlay *o, RomfsReadFn rd, void *ctx, uint64_t orig_size, uint64_t *size, char *why, size_t whycap);
ssize_t romfs_merged_pread(RomfsMerged *m, void *dst, size_t len, uint64_t off); /* bytes read (>0) or -1; extent-bounded like romfs_map_pread */
void romfs_merged_close(RomfsMerged *m);
/* Counters of the last build, for the log line: files replaced, files added, entries removed, overlay entries skipped (type conflict, unreadable). */
void romfs_merged_stats(const RomfsMerged *m, unsigned *replaced, unsigned *added, unsigned *removed, unsigned *skipped);
