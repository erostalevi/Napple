/* swap.h - disk-backed backing store for large, rarely touched guest memory (docs/architecture/memory-swap.md).
 *
 * A SwapFile is a sparse temp file that is unlinked as soon as it exists, so the space vanishes at exit, crash or jetsam. Regions of the guest
 * address space are mapped onto it MAP_SHARED: dirty file-backed pages are "external" to the kernel's footprint ledger and can be written back
 * and evicted, which anonymous memory cannot. Off by default; NAPPLE_IOS_SWAP=1 turns it on (any platform, the macOS path is the test bed). */
#pragma once
#include <stdint.h>

typedef struct SwapFile {
    int fd;
    uint64_t size;     /* file size, bytes */
    uint8_t *base;     /* host address of file offset 0 (the region the file backs), set by the first swap_commit */
    uint64_t high;     /* highest committed file offset + 1 */
    uint64_t committed; /* bytes currently mapped onto the file */
    uint64_t cursor;   /* rotating position of the periodic write-back */
    const char *name;
} SwapFile;

typedef struct SwapStats {
    uint64_t files, committed, resident, written_back, deactivated, trims;
} SwapStats;

int swap_enabled(void);                                                   /* NAPPLE_IOS_SWAP */
int swap_file_open(SwapFile *f, const char *name, uint64_t size);         /* 0 on success; the file is already unlinked */
void *swap_commit(SwapFile *f, uint8_t *region, uint64_t off, uint64_t len); /* maps [region+off, +len) onto the file at off; NULL on failure */
int swap_decommit(SwapFile *f, uint8_t *region, uint64_t off, uint64_t len); /* back to PROT_NONE anonymous, disk space released; 0 on success */
void *swap_map_anywhere(uint64_t len);                                    /* file-backed, zero-filled, any address (munmap to release); NULL when swap is off/failed */
void swap_trim(unsigned level);                                            /* 0 idle write-back slice, 1 memory warning, 2 critical */
void swap_stats(SwapStats *s);
