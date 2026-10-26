/* Size cap for the on-disk shader cache (the shaders folder of translated MSL, pipelines-v<N>.metalar and friends): oldest files go first. */
#ifndef NAPPLE_GFX_CACHE_TRIM_H
#define NAPPLE_GFX_CACHE_TRIM_H
#include <stdint.h>

#define GFX_CACHE_DEFAULT_MAX_MB 1024

/* Removes the least recently modified regular files under dir (recursively) until the total is at most max_bytes. The prewarm manifests
 * (pipelines.bin, pipelines-seed.bin) are never removed. max_bytes == 0 means unlimited. Returns the bytes removed (or, with dry_run, that
 * would be removed); *total_before, when not NULL, gets the size before trimming. */
uint64_t gfx_cache_trim(const char *dir, uint64_t max_bytes, int dry_run, uint64_t *total_before);

/* The cap in bytes: NAPPLE_GFX_CACHE_MAX_MB (0 = unlimited), default GFX_CACHE_DEFAULT_MAX_MB. */
uint64_t gfx_cache_max_bytes(void);
#endif
