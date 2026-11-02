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
    BCAT_STATUS_DONE = 9 /* DeliveryCacheProgressStatus::Done is 9 in the public ports (yuzu bcat_types.h), not the next number */
};

/* A directory name is 1..31 characters of [A-Za-z0-9_-], NUL padded. A file name additionally allows '.'. */
int bcat_valid_directory_name(const char name[BCAT_NAME_SIZE]);
int bcat_valid_file_name(const char name[BCAT_NAME_SIZE]);
/* DeliveryCacheProgressImpl: u32 status at +0, u32 result at +4, everything else 0 (nothing was transferred). */
void bcat_make_progress(uint32_t status, uint32_t result, uint8_t out[BCAT_PROGRESS_SIZE]);
/* Full DeliveryCacheProgressImpl (0x200 bytes), layout per switchbrew "BCAT services" (DeliveryCacheProgressImpl) and Ryujinx IDeliveryCacheProgressService, recalled
 * without network access: +0x00 u32 status, +0x04 u32 result, +0x08 char current_directory[0x20], +0x28 char current_file[0x20], +0x48 s64 current_downloaded,
 * +0x50 s64 current_total, +0x58 s64 total_downloaded, +0x60 s64 total_total, the rest reserved zero. The names are NUL terminated (at most 0x1f characters). */
#define BCAT_PROGRESS_DIR 0x08u
#define BCAT_PROGRESS_FILE 0x28u
#define BCAT_PROGRESS_CUR_DONE 0x48u
typedef struct {
    uint32_t status, result;
    const char *directory, *file; /* NULL = empty */
    int64_t current_downloaded, current_total, total_downloaded, total_total;
} BcatProgress;
void bcat_make_progress_full(const BcatProgress *p, uint8_t out[BCAT_PROGRESS_SIZE]);
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
    /* Optional (ipc-depth-95 r10): the delivery list the last sync downloaded (list.msgpack beside the directories): copies up to n bytes to buf (buf may be
     * NULL when n is 0) and returns the list's full size, or -1 when there is none. */
    int64_t (*read_list)(void *ctx, void *buf, size_t n);
} BcatProvider;
/* The process-wide provider (NULL clears it). Without one, NAPPLE_BCAT_DIR names a folder read at the first use: either the plain layout
 * <dir>/<directory>/<file>, or, when <dir>/directories.meta exists, the console's own delivery cache storage (bcat_save_provider). */
void bcat_set_provider(const BcatProvider *p);
const BcatProvider *bcat_provider(void);
/* A provider over a host folder: directories are its sub-folders, files their regular files (at most the limits above, valid names only). */
BcatProvider bcat_dir_provider(const char *root);
/* The console's delivery cache storage, as the BCAT service lays it out in the title's bcat save data (nn::bcat::detail::service::core::DeliveryCacheStorageManager
 * and the two meta accessors of LibHac / the public ports; read for this note):
 *   <root>/directories.meta                  u32 header = 1, then entries {char name[0x20]; u8 digest[0x10]; u8 reserved[0x10]} (0x40 each, at most 100)
 *   <root>/directories/<dir>/files.meta      u32 header = 1, then entries {char name[0x20]; s64 id; s64 size; u8 digest[0x10]; u8 reserved[0x40]} (0x80 each, at most 100)
 *   <root>/directories/<dir>/files/<file>    the content
 * (plus passphrase.bin, list.msgpack, etag.bin, na_required and index.lock beside them; only list.msgpack is read, by read_list, the others are ignored; the
 * plain folder layout's read_list reads <root>/list.msgpack too). The directory list, sizes and digests come
 * from the meta files; a listed file whose content is missing or shorter than its meta size reads as "gone" / short. */
enum { BCAT_META_HEADER = 1, BCAT_DIR_META_ENTRY = 0x40, BCAT_FILE_META_ENTRY = 0x80 };
BcatProvider bcat_save_provider(const char *root);
/* What NAPPLE_BCAT_DIR gets: the storage layout when <root>/directories.meta is a regular file, the plain folder layout otherwise. */
BcatProvider bcat_auto_provider(const char *root);
/* Writes one 0x38-byte DeliveryCacheDirectoryEntry: char name[0x20] at +0, s64 size at +0x20, u8 md5[0x10] at +0x28 (switchbrew "BCAT services"; Ryujinx's
 * DeliveryCacheFileMetaEntry is the same record). */
void bcat_pack_entry(const BcatFile *f, uint8_t out[BCAT_ENTRY_SIZE]);
/* A fingerprint of everything a title can read from the delivery cache (directory names, file names, sizes, digests), independent of listing order:
 * GetDeliveryCacheStorageUpdateNotifier compares it after each sync to tell whether the storage was updated. 0 without a provider. */
uint64_t bcat_storage_fingerprint(const BcatProvider *p);
void bcat_md5(const void *data, size_t n, uint8_t out[16]);
