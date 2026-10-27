/* bcat core: delivery-cache names, progress record and the cache provider hook (portable). With no provider installed the host has no delivery cache:
 * every directory is absent. A provider (the user's own folder via NAPPLE_BCAT_DIR, or a private build's downloader) supplies directories and files; this
 * repository carries no server code. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define BCAT_NAME_SIZE 0x20u
#define BCAT_PROGRESS_SIZE 0x200u
enum {
    BCAT_STATUS_NONE = 0,
    BCAT_STATUS_QUEUED = 1,
    BCAT_STATUS_CONNECTING = 2,
    BCAT_STATUS_PROCESSING_DATA_LIST = 3,
    BCAT_STATUS_DOWNLOADING = 4,
    BCAT_STATUS_COMMITTING = 5,
    BCAT_STATUS_DONE = 6
};

/* A directory name is 1..31 characters of [A-Za-z0-9_-], NUL padded. A file name additionally allows '.'. */
int bcat_valid_directory_name(const char name[BCAT_NAME_SIZE]);
int bcat_valid_file_name(const char name[BCAT_NAME_SIZE]);
/* DeliveryCacheProgressImpl: u32 status at +0, u32 result at +4, everything else 0 (nothing was transferred). */
void bcat_make_progress(uint32_t status, uint32_t result, uint8_t out[BCAT_PROGRESS_SIZE]);
/* A passphrase buffer holds at most 0x40 characters plus the NUL terminator. */
static inline int bcat_valid_passphrase_size(uint64_t size) { return size >= 1 && size <= 0x41; }

/* ---- delivery cache provider ---- */
#define BCAT_MAX_DIRS 100  /* directories a title can hold */
#define BCAT_MAX_FILES 100 /* files per directory */
#define BCAT_ENTRY_SIZE 0x38u
typedef struct {
    char name[BCAT_NAME_SIZE];
    uint64_t size;
    uint8_t digest[16]; /* MD5 of the content */
} BcatFile;
typedef struct {
    void *ctx;
    /* Each returns the count written (>= 0) or -1. Names are already valid (bcat_valid_*). */
    int (*list_dirs)(void *ctx, char names[][BCAT_NAME_SIZE], int max);
    int (*list_files)(void *ctx, const char *dir, BcatFile *out, int max);
    /* Bytes read at offset (0 at or past the end), or -1 when the file is gone. */
    int64_t (*read)(void *ctx, const char *dir, const char *file, uint64_t offset, void *buf, size_t n);
} BcatProvider;
/* The process-wide provider (NULL clears it). Without one, NAPPLE_BCAT_DIR names a folder laid out as <dir>/<directory>/<file>, read at the first use. */
void bcat_set_provider(const BcatProvider *p);
const BcatProvider *bcat_provider(void);
/* A provider over a host folder: directories are its sub-folders, files their regular files (at most the limits above, valid names only). */
BcatProvider bcat_dir_provider(const char *root);
void bcat_md5(const void *data, size_t n, uint8_t out[16]);
