/* fsp-srv: the game's file systems on macOS.
 *   RomFS     OpenDataStorageByCurrentProcess serves the raw RomFS image (<data-dir>/romfs.bin, the decrypted IVFC data level)
 *             as an IStorage: the SDK parses the RomFS tables itself, the host only answers reads.
 *   save data OpenSaveDataFileSystem maps to directories under <save-root>/<titleid>/save/<space>/<type>[/<user>].
 *   sdcard    <save-root>/<titleid>/sdcard.
 * Paths are validated (no "..", no NUL, bounded length) before they touch the host file system. */
#include "romfs_map.h"
#include "romfs_read.h"
#include "../ipc.h"
#include "host_info.h"
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef __APPLE__
#include <sys/clonefile.h>
#define st_created(sb) ((sb).st_birthtime)
#else /* the host unit tests (runtime/tests/host) build this file on Linux: no clones, the recursive copy is used */
static int clonefile(const char *src, const char *dst, int flags) {
    (void)src;
    (void)dst;
    (void)flags;
    errno = ENOTSUP;
    return -1;
}
#define st_created(sb) ((sb).st_ctime)
#endif
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
#include "sys_default.h"

#define FS_RESULT(desc) (2u | ((desc) << 9))
/* The host unit tests (runtime/tests/host) link this file without romfs_read.c: these fallbacks make the RomFS file system views unavailable there. */
__attribute__((weak)) RomfsImg *romfs_img_open(RomfsReadFn rd, void *ctx, uint64_t size) {
    (void)rd;
    (void)ctx;
    (void)size;
    return NULL;
}
__attribute__((weak)) void romfs_img_close(RomfsImg *img) { (void)img; }
__attribute__((weak)) int romfs_img_lookup(const RomfsImg *img, const char *path, uint64_t *off, uint64_t *size) {
    (void)img;
    (void)path;
    (void)off;
    (void)size;
    return ROMFS_NONE;
}
__attribute__((weak)) int romfs_img_child(const RomfsImg *img, const char *dir, unsigned n, char *name, size_t cap, int *is_dir, uint64_t *size) {
    (void)img;
    (void)dir;
    (void)n;
    (void)name;
    (void)cap;
    (void)is_dir;
    (void)size;
    return 0;
}
__attribute__((weak)) ssize_t romfs_img_read(const RomfsImg *img, uint64_t off, uint64_t fsize, uint64_t pos, void *dst, size_t len) {
    (void)img;
    (void)off;
    (void)fsize;
    (void)pos;
    (void)dst;
    (void)len;
    return -1;
}
#define R_FS_PATH_NOT_FOUND FS_RESULT(1)
#define R_FS_PATH_EXISTS FS_RESULT(2)
#define R_FS_TARGET_NOT_FOUND FS_RESULT(1002)
#define R_FS_TARGET_LOCKED FS_RESULT(7)
#define R_FS_DIR_NOT_EMPTY FS_RESULT(8)
#define R_FS_NOT_ENOUGH_SPACE FS_RESULT(3)
#define R_FS_INVALID_ARGUMENT FS_RESULT(6002)
#define R_FS_NOT_FOUND_DATA FS_RESULT(1001)
#define R_FS_UNEXPECTED FS_RESULT(5000)
#define R_FS_INVALID_ENUM FS_RESULT(6001) /* 0x2EE202: an enum value the service does not know (OpenCustomStorageFileSystem, OpenContentStorageFileSystem) */
/* ipc-depth-95 (r10). Values from switchbrew 'Error codes' (module 2): 2520 GameCardCardNotInserted (0x13B002), 3001 NotImplemented (0x177202, what the
 * Switch 2 firmware answers for OpenSaveDataTransferManager), and the range bases 6300 UnsupportedOperation (0x313802) and 6400 PermissionDenied
 * (0x320002). The range bases are [unverified] as the exact sub-code a console reports for these commands. */
#define R_FS_GAME_CARD_NOT_INSERTED FS_RESULT(2520)
#define R_FS_NOT_IMPLEMENTED FS_RESULT(3001)
#define R_FS_UNSUPPORTED_OPERATION FS_RESULT(6300)
#define R_FS_PERMISSION_DENIED FS_RESULT(6400)

static uint32_t errno_result(void) {
    switch (errno) {
        case ENOENT:
        case ENOTDIR: return R_FS_PATH_NOT_FOUND;
        case EEXIST: return R_FS_PATH_EXISTS;
        case ENOTEMPTY: return R_FS_DIR_NOT_EMPTY;
        case ENOSPC: return R_FS_NOT_ENOUGH_SPACE;
        case EBUSY:
        case ETXTBSY: return R_FS_TARGET_LOCKED;
        default: return R_FS_UNEXPECTED;
    }
}

/* ---- host path mapping ---------------------------------------------------------------------------------------- */
typedef struct SaveJournal {
    char live[1100], committed[1120], tmp[1120], old[1120], marker[1120];
    int ready;
    volatile int dirty;
    unsigned commits;
    pthread_mutex_t mu;
} SaveJournal;
static void journal_mark_dirty(SaveJournal *j) {
    if (j) j->dirty = 1;
}
typedef struct {
    char root[1024];
    int read_only;
    SaveJournal *journal;
    int64_t quota;
    int sd; /* the SD card file system (FAT on a console): QueryEntry, FAT open-entry peaks */
} HostFs; /* quota: save data size, 0 unknown */

/* Open files / directories of the SD card file system and their peaks since the last GetAndClearErrorInfo (the SD card's FatFatReportInfo1). */
static pthread_mutex_t g_sd_open_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_sd_open[2], g_sd_open_peak[2]; /* [0] files, [1] directories */
static void sd_open_track(int dir, int delta) {
    pthread_mutex_lock(&g_sd_open_mu);
    if (delta > 0 || g_sd_open[dir]) g_sd_open[dir] += (unsigned)delta;
    if (g_sd_open[dir] > g_sd_open_peak[dir]) g_sd_open_peak[dir] = g_sd_open[dir];
    pthread_mutex_unlock(&g_sd_open_mu);
}

/* Maps a guest path ("/dir/file", "dir/file") below the file system root; 0 when the path is not acceptable. */
static int host_path(const HostFs *fs, const char *guest, char *out, size_t cap) {
    if (!guest) return 0;
    size_t n = strnlen(guest, 0x302);
    if (n == 0 || n > 0x300) return 0;
    char norm[0x302];
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = guest[i] == '\\' ? '/' : guest[i];
        if (ch == ':' && i + 1 < n && guest[i + 1] == '/') {
            k = 0;
            continue;
        } /* "save:/x" style mount prefix */
        if (ch == '/' && k && norm[k - 1] == '/') continue;
        norm[k++] = ch;
    }
    norm[k] = 0;
    for (char *p = norm; *p;) { /* reject any ".." component */
        char *e = strchr(p, '/');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == 2 && p[0] == '.' && p[1] == '.') return 0;
        if (!e) break;
        p = e + 1;
    }
    const char *rel = norm[0] == '/' ? norm + 1 : norm;
    int w = snprintf(out, cap, "%s/%s", fs->root, rel);
    if (w < 0 || (size_t)w >= cap) return 0;
    while (w > 1 && out[w - 1] == '/') out[--w] = 0;
    return 1;
}
static const char *guest_path(const IpcCall *c, unsigned n, char *buf, size_t cap) {
    const IpcBuf *b = n < c->nx ? &c->x[n] : (n - c->nx < c->na ? &c->a[n - c->nx] : NULL);
    if (!b) return NULL;
    const char *p = ipc_buf_ptr(b);
    if (!p) return NULL;
    size_t len = b->size < cap - 1 ? b->size : cap - 1;
    memcpy(buf, p, len);
    buf[len] = 0;
    return buf;
}

static void mkdirs(const char *path) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    mkdir(tmp, 0755);
}

/* OperateRange(u32 operation id, s64 offset, s64 size): OperationId 0 Clear, 1 ClearSignature, 2 InvalidateCache, 3 QueryRange. A plain file or storage has
 * nothing to invalidate and no encrypted or emulated-speed range: InvalidateCache succeeds and QueryRange answers an all-zero 0x40-byte QueryRangeInfo (key
 * type 0, speed emulation 0). Clear zero-fills the range through `clear` when the object is writable and is refused by a read-only one. */
static void operate_range(IpcCall *c, int writable, int (*clear)(void *, int64_t, int64_t), void *ctx) {
    uint32_t op = IPC_IN(c, uint32_t, 0);
    int64_t off = IPC_IN(c, int64_t, 8), size = IPC_IN(c, int64_t, 16);
    if (off < 0 || size < 0) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    switch (op) {
        case 3: {
            uint8_t info[0x40] = {0};
            ipc_out(c, info, sizeof info);
            return;
        }
        case 2: return;
        case 0:
        case 1:
            if (!writable) {
                c->result = R_FS_INVALID_ARGUMENT;
                return;
            }
            if (op == 0 && clear && clear(ctx, off, size)) c->result = errno_result();
            return;
        default: c->result = R_FS_INVALID_ARGUMENT; return;
    }
}

/* ---- IFile --------------------------------------------------------------------------------------------------- */
typedef struct {
    int fd;
    int writable;
    char path[1100];
    SaveJournal *journal;
    int sd; /* counted in g_sd_open */
} HostFile;
static void file_destroy(IpcObject *o) {
    HostFile *f = o->state;
    if (f) {
        if (f->sd) sd_open_track(0, -1);
        if (f->fd >= 0) close(f->fd);
        free(f);
    }
}
static void file_read(IpcObject *self, IpcCall *c) {
    HostFile *f = self->state;
    int64_t off = IPC_IN(c, int64_t, 8), size = IPC_IN(c, int64_t, 16);
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!b || off < 0 || size < 0) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    if ((uint64_t)size > b->size) size = (int64_t)b->size;
    void *dst = guest_ptr(b->addr, (uint64_t)size);
    if (!dst) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    guest_write_prepare(b->addr, (size_t)size);
    ssize_t n = pread(f->fd, dst, (size_t)size, off);
    if (n < 0) {
        c->result = errno_result();
        return;
    }
    uint64_t got = (uint64_t)n;
    IPC_OUT(c, got);
}
static void file_write(IpcObject *self, IpcCall *c) {
    HostFile *f = self->state;
    int64_t off = IPC_IN(c, int64_t, 8), size = IPC_IN(c, int64_t, 16);
    const IpcBuf *b = ipc_in_buf(c, 0);
    if (!f->writable) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    if (!b || off < 0 || size < 0 || (uint64_t)size > b->size) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    const void *src = guest_ptr(b->addr, (uint64_t)size);
    if (!src) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    if (pwrite(f->fd, src, (size_t)size, off) != size) c->result = errno_result();
    else journal_mark_dirty(f->journal);
}
static void file_flush(IpcObject *self, IpcCall *c) {
    HostFile *f = self->state;
    if (f->writable) fsync(f->fd);
    (void)c;
}
static void file_set_size(IpcObject *self, IpcCall *c) {
    HostFile *f = self->state;
    journal_mark_dirty(f->journal);
    if (ftruncate(f->fd, IPC_IN(c, int64_t, 0))) c->result = errno_result();
}
static void file_get_size(IpcObject *self, IpcCall *c) {
    HostFile *f = self->state;
    struct stat sb;
    if (fstat(f->fd, &sb)) {
        c->result = errno_result();
        return;
    }
    int64_t v = sb.st_size;
    IPC_OUT(c, v);
}
static int host_file_clear(void *ctx, int64_t off, int64_t size) {
    HostFile *f = ctx;
    uint8_t zero[4096] = {0};
    journal_mark_dirty(f->journal);
    struct stat sb;
    if (fstat(f->fd, &sb)) return -1;
    if (off >= sb.st_size) return 0;
    if (size > sb.st_size - off) size = sb.st_size - off;
    for (int64_t done = 0; done < size;) {
        int64_t n = size - done < (int64_t)sizeof zero ? size - done : (int64_t)sizeof zero;
        if (pwrite(f->fd, zero, (size_t)n, off + done) != n) return -1;
        done += n;
    }
    return 0;
}
static void file_operate_range(IpcObject *self, IpcCall *c) {
    HostFile *f = self->state;
    operate_range(c, f->writable, host_file_clear, f);
}
static const IpcCmd file_cmds[] = {
    {0, "Read", file_read, IPC_IMPL},        {1, "Write", file_write, IPC_IMPL},      {2, "Flush", file_flush, IPC_IMPL},
    {3, "SetSize", file_set_size, IPC_IMPL}, {4, "GetSize", file_get_size, IPC_IMPL},
    {5, "OperateRange", file_operate_range, IPC_IMPL},
    {6, "OperateRangeWithBuffer", file_operate_range, IPC_IMPL},
};

/* ---- IDirectory ---------------------------------------------------------------------------------------------- */
typedef struct {
    DIR *d;
    int mode;
    char path[1100];
    int sd; /* counted in g_sd_open */
} HostDir;
static void dir_destroy(IpcObject *o) {
    HostDir *d = o->state;
    if (d) {
        if (d->sd) sd_open_track(1, -1);
        closedir(d->d);
        free(d);
    }
}
static void dir_read(IpcObject *self, IpcCall *c) {
    HostDir *d = self->state;
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!b) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    uint8_t *out = guest_ptr(b->addr, b->size);
    if (!out) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    int64_t max = (int64_t)(b->size / 0x310), n = 0;
    struct dirent *e;
    guest_write_prepare(b->addr, (size_t)b->size);
    while (n < max && (e = readdir(d->d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        int isdir = e->d_type == DT_DIR;
        if ((isdir && !(d->mode & 1)) || (!isdir && !(d->mode & 2))) continue;
        uint8_t *ent = out + n * 0x310;
        memset(ent, 0, 0x310);
        strncpy((char *)ent, e->d_name, 0x300);
        ent[0x304] = isdir ? 0 : 1;
        if (!isdir) {
            char p[1400];
            struct stat sb;
            snprintf(p, sizeof p, "%s/%s", d->path, e->d_name);
            if (!stat(p, &sb)) {
                int64_t sz = sb.st_size;
                memcpy(ent + 0x308, &sz, 8);
            }
        }
        n++;
    }
    IPC_OUT(c, n);
}
static void dir_entry_count(IpcObject *self, IpcCall *c) {
    HostDir *d = self->state;
    long pos = telldir(d->d);
    rewinddir(d->d);
    int64_t n = 0;
    struct dirent *e;
    while ((e = readdir(d->d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        int isdir = e->d_type == DT_DIR;
        if ((isdir && (d->mode & 1)) || (!isdir && (d->mode & 2))) n++;
    }
    seekdir(d->d, pos);
    IPC_OUT(c, n);
}
static const IpcCmd dir_cmds[] = {{0, "Read", dir_read, IPC_IMPL}, {1, "GetEntryCount", dir_entry_count, IPC_IMPL}};

/* ---- IFileSystem --------------------------------------------------------------------------------------------- */
static int rm_rf(const char *p, int keep_root) {
    DIR *d = opendir(p);
    if (!d) return unlink(p);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[1400];
        snprintf(child, sizeof child, "%s/%s", p, e->d_name);
        rm_rf(child, 0);
    }
    closedir(d);
    return keep_root ? 0 : rmdir(p);
}
/* ---- save data commit semantics --------------------------------------------------------------------------------
 * Horizon save data is transactional: writes are private to the process until nn::fs::CommitSaveData (IFileSystem::Commit); a crash or
 * a quit before the commit leaves the previously committed state. The host directory (<root>) is the live working copy the game writes
 * into; `<root>.committed` is the last committed snapshot (an APFS clone, a recursive copy elsewhere); `.napple-journal` next to it marks
 * a root that is under this scheme.
 *   first open in a process (recover): finish an interrupted swap, discard stale temporaries, then live := clone(committed)  (rolls back
 *     uncommitted work of a previous run); a root without marker and snapshot is a pre-journal save: live becomes the first snapshot.
 *   Commit: tmp := clone(live); committed -> .old; tmp -> committed; remove .old; sync.  A crash at any point leaves either the old or
 *     the new snapshot (recover renames .old back when committed is missing).
 * Open files keep pointing at the live directory, so a game that keeps a save file open across a commit still works. */
static SaveJournal *g_journals[16];
static pthread_mutex_t g_journals_mu = PTHREAD_MUTEX_INITIALIZER;
static int path_exists(const char *p) {
    struct stat sb;
    return lstat(p, &sb) == 0;
}
static int copy_tree(const char *src, const char *dst) { /* fallback when clonefile is unavailable (not APFS) */
    struct stat sb;
    if (lstat(src, &sb)) return -1;
    if (S_ISDIR(sb.st_mode)) {
        if (mkdir(dst, 0755) && errno != EEXIST) return -1;
        DIR *d = opendir(src);
        if (!d) return -1;
        struct dirent *e;
        int rc = 0;
        while (!rc && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char a[1400], b[1400];
            snprintf(a, sizeof a, "%s/%s", src, e->d_name);
            snprintf(b, sizeof b, "%s/%s", dst, e->d_name);
            rc = copy_tree(a, b);
        }
        closedir(d);
        return rc;
    }
    int in = open(src, O_RDONLY), out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int rc = in < 0 || out < 0 ? -1 : 0;
    char buf[65536];
    ssize_t n;
    while (!rc && (n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) rc = -1;
    if (n < 0) rc = -1;
    if (in >= 0) close(in);
    if (out >= 0 && (fsync(out) || close(out))) rc = -1;
    return rc;
}
static int clone_tree(const char *src, const char *dst) { return clonefile(src, dst, 0) == 0 ? 0 : copy_tree(src, dst); }
static void journal_recover(SaveJournal *j) {
    if (!path_exists(j->committed) && path_exists(j->old)) rename(j->old, j->committed); /* crash between the two renames of a commit */
    rm_rf(j->tmp, 0);
    rm_rf(j->old, 0);
    if (path_exists(j->committed)) {
        /* live := clone(committed), built beside the live directory first: if the clone fails (permissions, disk full) the existing directory stays
         * untouched instead of being lost with nothing to replace it */
        char staging[1130];
        snprintf(staging, sizeof staging, "%s.restoring", j->live);
        rm_rf(staging, 0);
        if (clone_tree(j->committed, staging)) {
            LOG("fs", "save %s: cannot restore the committed state: %s (keeping the directory as it is)", j->live, strerror(errno));
            rm_rf(staging, 0);
        } else {
            rm_rf(j->live, 0);
            if (rename(staging, j->live)) LOG("fs", "save %s: cannot move the restored state into place: %s", j->live, strerror(errno));
            else LOGV("fs", "save %s: restored from the last committed snapshot", j->live);
        }
    } else if (!path_exists(j->marker) && path_exists(j->live)) { /* pre-journal save: adopt it */
        if (clone_tree(j->live, j->committed)) LOG("fs", "save %s: cannot take the first snapshot: %s", j->live, strerror(errno));
    } else {
        rm_rf(j->live, 0);
    } /* marker but nothing ever committed */
    mkdirs(j->live);
    int fd = open(j->marker, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);
}
static SaveJournal *journal_get(const char *live) {
    pthread_mutex_lock(&g_journals_mu);
    SaveJournal *j = NULL;
    for (int i = 0; i < 16 && g_journals[i]; i++)
        if (!strcmp(g_journals[i]->live, live)) j = g_journals[i];
    if (!j) {
        for (int i = 0; i < 16; i++)
            if (!g_journals[i]) {
                j = calloc(1, sizeof *j);
                pthread_mutex_init(&j->mu, NULL);
                g_journals[i] = j;
                snprintf(j->live, sizeof j->live, "%s", live);
                snprintf(j->committed, sizeof j->committed, "%s.committed", live);
                snprintf(j->tmp, sizeof j->tmp, "%s.committing", live);
                snprintf(j->old, sizeof j->old, "%s.previous", live);
                snprintf(j->marker, sizeof j->marker, "%s.napple-journal", live);
                break;
            }
    }
    pthread_mutex_unlock(&g_journals_mu);
    if (!j) LOG("fs", "save %s: more than 16 transactional save roots; this one is not transactional", live);
    if (j) {
        pthread_mutex_lock(&j->mu);
        if (!j->ready) {
            char parent[1100];
            snprintf(parent, sizeof parent, "%s", live);
            char *sl = strrchr(parent, '/');
            if (sl) {
                *sl = 0;
                mkdirs(parent);
            }
            journal_recover(j);
            j->ready = 1;
        }
        pthread_mutex_unlock(&j->mu);
    }
    return j;
}
static uint32_t journal_commit(SaveJournal *j) {
    pthread_mutex_lock(&j->mu);
    uint32_t r = 0;
    int was_dirty = j->dirty;
    if (!was_dirty && path_exists(j->committed)) { /* nothing was written since the last snapshot: a commit changes nothing (and a clone plus sync() is tens of ms: games commit after
                                                      every probe, and a slow commit lets their other threads run ahead of the committing one) */
        j->commits++;
        pthread_mutex_unlock(&j->mu);
        return 0;
    }
    j->dirty = 0; /* before the snapshot: a write racing with the commit marks the root dirty again */
    rm_rf(j->tmp, 0);
    if (clone_tree(j->live, j->tmp)) {
        r = errno_result();
        LOG("fs", "commit %s: snapshot failed: %s", j->live, strerror(errno));
        rm_rf(j->tmp, 0);
    } else {
        if (path_exists(j->committed) && rename(j->committed, j->old)) {
            r = errno_result();
            rm_rf(j->tmp, 0);
        } else if (rename(j->tmp, j->committed)) {
            r = errno_result();
            if (path_exists(j->old)) rename(j->old, j->committed);
            rm_rf(j->tmp, 0);
        } else rm_rf(j->old, 0);
    }
    if (r) j->dirty = j->dirty || was_dirty;
    else j->commits++;
    pthread_mutex_unlock(&j->mu);
    if (!r) sync();
    return r;
}

/* ---- save data metadata: ids, extra data, sizes --------------------------------------------------------------------------
 * Horizon keeps per save data a SaveDataExtraData record (attribute, owner, flags, sizes, timestamp, commit id) and a save data id. The host
 * keeps them in `<root>.napple-meta` next to the save directory: "NSMD", u32 version, u64 save data id, u8 space, u8 implicit, 6 pad, extra data
 * (0x200).
 * The id is derived from the root path (stable across runs). A save that exists without a meta file (opened before it was created, or
 * from an older runtime) gets one on first use, with sizes 0 (unknown). */
typedef struct {
    uint8_t attr[0x40];
    uint64_t owner_id;
    int64_t timestamp;
    uint32_t flags, pad;
    int64_t data_size, journal_size, commit_id;
    uint8_t unused[0x190];
} SaveExtraData;
_Static_assert(sizeof(SaveExtraData) == 0x200, "SaveDataExtraData layout");
typedef struct {
    char magic[4];
    uint32_t version;
    uint64_t id;
    uint8_t space, implicit, pad[6];
    SaveExtraData extra;
} SaveMeta; /* implicit: made on first use, not by Create */
#define META_SUFFIX ".napple-meta"
enum { SAVE_SYSTEM = 0, SAVE_ACCOUNT = 1, SAVE_BCAT = 2, SAVE_DEVICE = 3, SAVE_TEMPORARY = 4, SAVE_CACHE = 5 };
static pthread_mutex_t g_meta_mu = PTHREAD_MUTEX_INITIALIZER;

/* <save>/space<N>/<type>[/<system save id>][/<user>][/idx<index>] for SaveDataAttribute {u64 program, u128 user, u64 system id, u8 type,
 * u8 rank, u16 index}. */
static void save_root(uint8_t space, const uint8_t *attr, char *root, size_t cap) {
    static const char *const types[] = {"system", "account", "bcat", "device", "temporary", "cache"};
    uint8_t type = attr[0x20];
    uint16_t index;
    memcpy(&index, attr + 0x22, 2);
    uint64_t sid;
    memcpy(&sid, attr + 0x18, 8);
    const uint8_t *uid = attr + 8;
    int has_uid = 0;
    for (int i = 0; i < 16; i++)
        if (uid[i]) has_uid = 1;
    int n = snprintf(root, cap, "%s/space%u/%s", host_save_dir(), space, type < 6 ? types[type] : "other");
    if (type == SAVE_SYSTEM && sid) n += snprintf(root + n, cap - (size_t)n, "/%016llx", (unsigned long long)sid);
    if (has_uid) n += snprintf(root + n, cap - (size_t)n, "/%02x%02x%02x%02x", uid[0], uid[1], uid[2], uid[3]);
    if (index) snprintf(root + n, cap - (size_t)n, "/idx%u", index);
}
static uint64_t save_id_for(const char *root) {
    const char *rel = root + strlen(host_save_dir());
    uint64_t h = 1469598103934665603ull;
    for (; *rel; rel++) h = (h ^ (uint8_t)*rel) * 1099511628211ull;
    return (h & 0x0000ffffffffffffull) | 0x0100000000000000ull; /* never 0 (0 means "no id" to fs) */
}
static int meta_read(const char *root, SaveMeta *m) {
    char p[1300];
    snprintf(p, sizeof p, "%s" META_SUFFIX, root);
    int fd = open(p, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, m, sizeof *m);
    close(fd);
    return n == (ssize_t)sizeof *m && !memcmp(m->magic, "NSMD", 4);
}
static int meta_write(const char *root, const SaveMeta *m) {
    char p[1300], tmp[1320];
    snprintf(p, sizeof p, "%s" META_SUFFIX, root);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) return -1;
    int ok = write(fd, m, sizeof *m) == (ssize_t)sizeof *m && !fsync(fd);
    close(fd);
    if (!ok || rename(tmp, p)) {
        unlink(tmp);
        return -1;
    } /* atomic replace: a crash keeps the old record */
    return 0;
}
static void meta_init(SaveMeta *m, const char *root, uint8_t space, const uint8_t *attr) {
    memset(m, 0, sizeof *m);
    memcpy(m->magic, "NSMD", 4);
    m->version = 1;
    m->id = save_id_for(root);
    m->space = space;
    memcpy(m->extra.attr, attr, 0x40);
    m->extra.timestamp = (int64_t)time(NULL);
}
/* Meta of an existing save root (created with defaults when the directory exists without one). 0 when there is no such save. */
static int meta_get(const char *root, uint8_t space, const uint8_t *attr, SaveMeta *m) {
    if (meta_read(root, m)) return 1;
    struct stat sb;
    if (stat(root, &sb) || !S_ISDIR(sb.st_mode)) return 0;
    meta_init(m, root, space, attr);
    m->implicit = 1;
    return meta_write(root, m) == 0;
}
/* Walks the save tree for meta files; calls fn(root, meta, arg) for each until it returns nonzero. */
static int meta_walk(const char *dir, int depth, int (*fn)(const char *, const SaveMeta *, void *), void *arg) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int stop = 0;
    while (!stop && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1400];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        size_t n = strlen(p), k = strlen(META_SUFFIX);
        if (n > k && !strcmp(p + n - k, META_SUFFIX)) {
            p[n - k] = 0;
            SaveMeta m;
            if (meta_read(p, &m)) stop = fn(p, &m, arg);
            continue;
        }
        struct stat sb;
        if (depth > 0 && !lstat(p, &sb) && S_ISDIR(sb.st_mode) && !strstr(e->d_name, ".committed") && !strstr(e->d_name, ".previous") &&
            !strstr(e->d_name, ".committing"))
            stop = meta_walk(p, depth - 1, fn, arg);
    }
    closedir(d);
    return stop;
}
typedef struct {
    uint64_t id;
    int space;
    char root[1300];
    SaveMeta meta;
    int found;
} FindById;
static int find_by_id_cb(const char *root, const SaveMeta *m, void *arg) {
    FindById *f = arg;
    if (m->id != f->id || (f->space >= 0 && m->space != f->space)) return 0;
    snprintf(f->root, sizeof f->root, "%s", root);
    f->meta = *m;
    f->found = 1;
    return 1;
}
static int find_by_id(int space, uint64_t id, char *root, size_t cap, SaveMeta *m) {
    FindById f = {.id = id, .space = space};
    meta_walk(host_save_dir(), 6, find_by_id_cb, &f);
    if (f.found) {
        snprintf(root, cap, "%s", f.root);
        *m = f.meta;
    }
    return f.found;
}
/* Directory usage in bytes, rounded to 16 KiB blocks per file (how save data counts its space). */
static int64_t tree_usage(const char *p) {
    struct stat sb;
    if (lstat(p, &sb)) return 0;
    if (!S_ISDIR(sb.st_mode)) return (sb.st_size + 0x3fff) & ~0x3fffll;
    int64_t total = 0;
    DIR *d = opendir(p);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[1400];
        snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        total += tree_usage(c);
    }
    closedir(d);
    return total;
}
static int save_type_journaled(uint8_t type) { return type == SAVE_SYSTEM || type == SAVE_ACCOUNT || type == SAVE_DEVICE || type == SAVE_CACHE; }
/* Removes a save root with its snapshot, journal marker and meta; the in-process journal starts over if the root is created again. */
static void save_delete(const char *root) {
    char p[1400];
    rm_rf(root, 0);
    snprintf(p, sizeof p, "%s.committed", root);
    rm_rf(p, 0);
    snprintf(p, sizeof p, "%s.napple-journal", root);
    unlink(p);
    snprintf(p, sizeof p, "%s" META_SUFFIX, root);
    unlink(p);
    for (unsigned t = 1; t <= 2; t++) {
        snprintf(p, sizeof p, "%s.napple-savemeta%u", root, t);
        unlink(p);
    }
    pthread_mutex_lock(&g_journals_mu);
    for (int i = 0; i < 16 && g_journals[i]; i++)
        if (!strcmp(g_journals[i]->live, root)) {
            g_journals[i]->ready = 0;
            g_journals[i]->dirty = 0;
        }
    pthread_mutex_unlock(&g_journals_mu);
}

#define FS_PATH(var, n)                                                                                                                                        \
    char gp##n[0x302], var[1400];                                                                                                                              \
    if (!guest_path(c, n, gp##n, sizeof gp##n) || !host_path(fs, gp##n, var, sizeof var)) {                                                                    \
        c->result = R_FS_INVALID_ARGUMENT;                                                                                                                     \
        return;                                                                                                                                                \
    }
static int fs_trace(void) {
    static int v = -1;
    if (v < 0) v = getenv("NAPPLE_FS_TRACE") != NULL;
    return v;
}
#define FS_WRITE_CHECK()                                                                                                                                       \
    if (fs->read_only) {                                                                                                                                       \
        c->result = R_FS_INVALID_ARGUMENT;                                                                                                                     \
        return;                                                                                                                                                \
    }

static void fs_create_file(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state;
    FS_WRITE_CHECK();
    journal_mark_dirty(fs->journal);
    FS_PATH(p, 0);
    int fd = open(p, O_CREAT | O_EXCL | O_RDWR, 0644);
    if (fs_trace()) LOG("fs", "CreateFile %s -> %s", p, fd < 0 ? strerror(errno) : "ok");
    if (fd < 0) {
        c->result = errno_result();
        return;
    }
    if (ftruncate(fd, IPC_IN(c, int64_t, 8))) c->result = errno_result();
    close(fd);
}
static void fs_delete_file(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state;
    FS_WRITE_CHECK();
    journal_mark_dirty(fs->journal);
    FS_PATH(p, 0);
    if (fs_trace()) LOG("fs", "DeleteFile %s", p);
    if (unlink(p)) c->result = errno_result();
}
static void fs_create_directory(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state;
    FS_WRITE_CHECK();
    journal_mark_dirty(fs->journal);
    FS_PATH(p, 0);
    if (mkdir(p, 0755)) c->result = errno_result();
}
static void fs_delete_directory(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state;
    FS_WRITE_CHECK();
    journal_mark_dirty(fs->journal);
    FS_PATH(p, 0);
    if (rmdir(p)) c->result = errno_result();
}
static void fs_delete_directory_recursively(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state;
    FS_WRITE_CHECK();
    journal_mark_dirty(fs->journal);
    FS_PATH(p, 0);
    if (rm_rf(p, 0)) c->result = errno_result();
}
static void fs_rename(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state;
    FS_WRITE_CHECK();
    journal_mark_dirty(fs->journal);
    FS_PATH(p, 0);
    FS_PATH(q, 1);
    if (fs_trace()) LOG("fs", "Rename %s -> %s", p, q);
    if (rename(p, q)) c->result = errno_result();
}
static void fs_get_entry_type(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state;
    FS_PATH(p, 0);
    struct stat sb;
    int sr = stat(p, &sb);
    if (fs_trace()) LOG("fs", "GetEntryType %s -> %s (thread %#llx)", p, sr ? strerror(errno) : S_ISDIR(sb.st_mode) ? "dir" : "file", (unsigned long long)threads_current_id());
    if (sr) {
        c->result = R_FS_PATH_NOT_FOUND;
        return;
    }
    uint32_t t = S_ISDIR(sb.st_mode) ? 0 : 1;
    IPC_OUT(c, t);
}
static void fs_open_file(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state;
    FS_PATH(p, 0);
    uint32_t mode = IPC_IN(c, uint32_t, 0);
    int writable = (mode & 6) != 0;
    if (writable && fs->read_only) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    int fd = open(p, writable ? O_RDWR : O_RDONLY);
    if (fs_trace()) LOG("fs", "OpenFile %s mode %u -> %s (thread %#llx)", p, mode, fd < 0 ? strerror(errno) : "ok", (unsigned long long)threads_current_id());
    if (fd < 0) {
        c->result = errno_result();
        return;
    }
    HostFile *f = calloc(1, sizeof *f);
    f->fd = fd;
    f->writable = writable;
    f->journal = fs->journal;
    snprintf(f->path, sizeof f->path, "%s", p);
    if ((f->sd = fs->sd)) sd_open_track(0, 1);
    ipc_out_object(c, IPC_OBJECT("IFile", file_cmds, f, file_destroy));
}
static void fs_open_directory(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state;
    FS_PATH(p, 0);
    DIR *d = opendir(p);
    if (!d) {
        c->result = errno_result();
        return;
    }
    HostDir *hd = calloc(1, sizeof *hd);
    hd->d = d;
    hd->mode = (int)IPC_IN(c, uint32_t, 0);
    snprintf(hd->path, sizeof hd->path, "%s", p);
    if ((hd->sd = fs->sd)) sd_open_track(1, 1);
    ipc_out_object(c, IPC_OBJECT("IDirectory", dir_cmds, hd, dir_destroy));
}
/* Every commit of a save gives it a new commit id (GetSaveDataCommitId, SaveDataExtraData.commit_id): strictly increasing, seeded from the clock. */
static void meta_bump_commit_id(const char *root) {
    SaveMeta m;
    pthread_mutex_lock(&g_meta_mu);
    if (meta_read(root, &m)) {
        int64_t now = (int64_t)time(NULL) << 16;
        m.extra.commit_id = now > m.extra.commit_id ? now : m.extra.commit_id + 1;
        meta_write(root, &m);
    }
    pthread_mutex_unlock(&g_meta_mu);
}
/* Cloud saves hooks (runtime/src/cloud/cloud.c): a commit that took a new snapshot schedules a debounced background push; both clean exits
 * (fs_report_uncommitted) push what is pending. Weak no-ops here so builds without runtime/src/cloud (CLOUD=0, tools/build_ios.sh) link. */
__attribute__((weak)) void cloud_on_commit(const char *root) { (void)root; }
__attribute__((weak)) void cloud_on_exit(void) {}
static void fs_commit(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state;
    if (fs_trace()) LOG("fs", "Commit %s%s (thread %#llx)", fs->root, fs->journal ? (fs->journal->dirty ? " (changes pending)" : " (clean)") : " (not transactional)",
                            (unsigned long long)threads_current_id());
    if (fs->journal) {
        int had_changes = fs->journal->dirty;
        uint32_t r = journal_commit(fs->journal);
        if (r) c->result = r;
        else {
            meta_bump_commit_id(fs->root);
            if (had_changes) cloud_on_commit(fs->root);
        }
    } else sync();
}
/* Sizes: a save created with a size answers from it (free = size - blocks in use); everything else reports 1 GiB. */
#define FS_DEFAULT_SPACE 0x40000000ll
static void fs_free_space(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state;
    int64_t v = FS_DEFAULT_SPACE;
    if (fs->quota > 0) {
        v = fs->quota - tree_usage(fs->root);
        if (v < 0) v = 0;
    }
    IPC_OUT(c, v);
}
static void fs_total_space(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state;
    int64_t v = fs->quota > 0 ? fs->quota : FS_DEFAULT_SPACE;
    IPC_OUT(c, v);
}
static void fs_clean_directory(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state;
    FS_WRITE_CHECK();
    journal_mark_dirty(fs->journal);
    FS_PATH(p, 0);
    rm_rf(p, 1);
}
static void fs_get_timestamp(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state;
    FS_PATH(p, 0);
    struct stat sb;
    if (stat(p, &sb)) {
        c->result = R_FS_PATH_NOT_FOUND;
        return;
    }
    int64_t t[3] = {st_created(sb), sb.st_mtime, sb.st_atime};
    uint8_t local[8] = {0};
    ipc_out(c, t, sizeof t);
    ipc_out(c, local, 8);
}
/* SetConcatenationFileAttribute on the host: on a console the archive bit turns a FAT directory into one ConcatenationFile whose data is its chunk files
 * ("00", "01", ... in order [unverified names]) laid end to end, so GetEntryType answers File and OpenFile reads the joined data. The host file system has
 * no 4 GiB file limit, so the faithful result is that file: the chunks are joined into one host file that replaces the directory. A directory holding
 * anything but a contiguous run of chunk files is refused (InvalidArgument); a path that is already a file stays as it is. */
static uint32_t concat_directory(const char *p) {
    struct stat sb;
    if (stat(p, &sb)) return R_FS_PATH_NOT_FOUND;
    if (!S_ISDIR(sb.st_mode)) return 0;
    unsigned n = 0, seen[256] = {0};
    DIR *d = opendir(p);
    if (!d) return errno_result();
    struct dirent *e;
    uint32_t r = 0;
    while (!r && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *end;
        unsigned long k = strtoul(e->d_name, &end, 10);
        if (!*e->d_name || *end || k >= 256 || seen[k]) r = R_FS_INVALID_ARGUMENT;
        else seen[k] = 1, n++;
    }
    closedir(d);
    for (unsigned k = 0; !r && k < n; k++)
        if (!seen[k]) r = R_FS_INVALID_ARGUMENT;
    if (r) return r;
    char tmp[1450];
    snprintf(tmp, sizeof tmp, "%s.napple-concat", p);
    int out = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (out < 0) return errno_result();
    for (unsigned k = 0; !r && k < n; k++) {
        char cp[1450];
        snprintf(cp, sizeof cp, "%s/%02u", p, k);
        int in = open(cp, O_RDONLY);
        if (in < 0) { /* "0", "1": accept the unpadded spelling too */
            snprintf(cp, sizeof cp, "%s/%u", p, k);
            in = open(cp, O_RDONLY);
        }
        if (in < 0) {
            r = errno_result();
            break;
        }
        char buf[65536];
        ssize_t got;
        while ((got = read(in, buf, sizeof buf)) > 0)
            if (write(out, buf, (size_t)got) != got) {
                r = errno_result();
                break;
            }
        if (got < 0) r = errno_result();
        close(in);
    }
    close(out);
    if (!r && (rm_rf(p, 0) || rename(tmp, p))) r = errno_result();
    if (r) unlink(tmp);
    return r;
}
/* 15 QueryEntry(u32 query id; in path (X), in buffer, out buffer). FileSystemQueryId (libnx fs.h): 0 SetConcatenationFileAttribute [4.0.0+],
 * 2 IsValidSignedSystemPartitionOnSdCard [8.0.0+]. Both belong to the SD card file system (FAT); save data and the other host-backed file systems answer
 * UnsupportedOperation, as does any other query id. The SD card directory never holds a signed system partition (no system update is ever written to
 * it), so query 2 answers false. */
static void fs_query_entry(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state;
    uint32_t q = c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0xffffffffu;
    if (!fs->sd || (q != 0 && q != 2)) {
        c->result = R_FS_UNSUPPORTED_OPERATION;
        return;
    }
    FS_PATH(p, 0);
    if (q == 0) {
        FS_WRITE_CHECK();
        uint32_t r = concat_directory(p);
        if (r) c->result = r;
        return;
    }
    struct stat sb;
    if (stat(p, &sb)) {
        c->result = R_FS_PATH_NOT_FOUND;
        return;
    }
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint8_t valid = 0;
    if (b && b->size) ipc_write_buf(c, b, &valid, 1);
}
/* 16 GetFileSystemAttribute -> FileSystemAttribute (0xC0, libnx fs.h FsFileSystemAttribute: 13 has-value flags, 0x1B reserved, 13 s32 limits, 0x64
 * reserved). The limits are the ones this file system enforces: host_path takes a path of up to 0x300 bytes (an FspPath without its NUL) and the host
 * directory takes entry names of up to NAME_MAX bytes. No UTF-16 limit exists here, so those flags stay clear. */
static void fs_get_attribute(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t a[0xC0] = {0};
    int32_t lim[4] = {NAME_MAX, NAME_MAX, 0x300, 0x300}; /* directory name, file name, directory path, file path */
    for (int i = 0; i < 4; i++) a[i] = 1;
    memcpy(a + 0x28, lim, sizeof lim);
    ipc_out(c, a, sizeof a);
}
static const IpcCmd fs_cmds[] = {
    {0, "CreateFile", fs_create_file, IPC_IMPL},
    {1, "DeleteFile", fs_delete_file, IPC_IMPL},
    {2, "CreateDirectory", fs_create_directory, IPC_IMPL},
    {3, "DeleteDirectory", fs_delete_directory, IPC_IMPL},
    {4, "DeleteDirectoryRecursively", fs_delete_directory_recursively, IPC_IMPL},
    {5, "RenameFile", fs_rename, IPC_IMPL},
    {6, "RenameDirectory", fs_rename, IPC_IMPL},
    {7, "GetEntryType", fs_get_entry_type, IPC_IMPL},
    {8, "OpenFile", fs_open_file, IPC_IMPL},
    {9, "OpenDirectory", fs_open_directory, IPC_IMPL},
    {10, "Commit", fs_commit, IPC_IMPL},
    {11, "GetFreeSpaceSize", fs_free_space, IPC_IMPL},
    {12, "GetTotalSpaceSize", fs_total_space, IPC_IMPL},
    {13, "CleanDirectoryRecursively", fs_clean_directory, IPC_IMPL},
    {14, "GetFileTimeStampRaw", fs_get_timestamp, IPC_IMPL},
    {15, "QueryEntry", fs_query_entry, IPC_IMPL},
    {16, "GetFileSystemAttribute", fs_get_attribute, IPC_IMPL},
};
static IpcObject *open_host_fs(const char *root, int read_only, int journaled, int64_t quota) {
    HostFs *fs = calloc(1, sizeof *fs);
    snprintf(fs->root, sizeof fs->root, "%s", root);
    fs->read_only = read_only;
    fs->quota = quota;
    if (journaled) fs->journal = journal_get(root);
    mkdirs(root);
    LOG("fs", "mounted host directory %s%s", root, read_only ? " (read only)" : "");
    return IPC_OBJECT("IFileSystem", fs_cmds, fs, NULL);
}

/* ---- IStorage over romfs.bin ------------------------------------------------------------------------------------ */
/* A BKTR delta layer (napple build --update-romfs delta): <data-dir>/romfs.delta/{romfs.json, romfs.reloc, patch.bin} over the base build's romfs.bin.
 * RLC1 reloc: 'RLC1', u32 count, u64 data_size, then {u64 data_offset, u64 source_offset, u32 source (0 base romfs.bin, 1 patch.bin), u32 0};
 * entry i covers [data_offset_i, data_offset_i+1). Reads are composed through the table. */
typedef struct {
    uint64_t data_offset, source_offset;
    uint32_t source;
} DeltaEnt;
typedef struct {
    int fd;
    uint64_t size;
    int src_fd[2];
    DeltaEnt *ents;
    uint32_t nents;
    RomfsMap *map;
    RomfsMerged *merged; /* mod overlay applied to the tables (virtual image); size is then the merged size */
} StorageFile;
static void storage_destroy(IpcObject *o) {
    StorageFile *s = o->state;
    if (!s) return;
    romfs_merged_close(s->merged);
    if (s->map) romfs_map_close(s->map);
    else if (s->ents) {
        for (int i = 0; i < 2; i++)
            if (s->src_fd[i] >= 0) close(s->src_fd[i]);
        free(s->ents);
    } else close(s->fd);
    free(s);
}
static ssize_t storage_base_pread(StorageFile *st, void *dst, size_t len, uint64_t off) {
    if (st->map) return romfs_map_pread(st->map, dst, len, off);
    if (!st->ents) return pread(st->fd, dst, len, (off_t)off);
    uint32_t lo = 0, hi = st->nents; /* last entry with data_offset <= off */
    while (hi - lo > 1) {
        uint32_t m = lo + (hi - lo) / 2;
        if (st->ents[m].data_offset <= off) lo = m;
        else hi = m;
    }
    const DeltaEnt *e = &st->ents[lo];
    uint64_t end = lo + 1 < st->nents ? st->ents[lo + 1].data_offset : st->size;
    if (off >= end || e->source > 1) return -1;
    size_t n = len < end - off ? len : (size_t)(end - off);
    return pread(st->src_fd[e->source], dst, n, (off_t)(e->source_offset + (off - e->data_offset)));
}
static ssize_t storage_base_read_cb(void *ctx, void *dst, size_t len, uint64_t off) { return storage_base_pread(ctx, dst, len, off); }
static ssize_t storage_pread(StorageFile *st, void *dst, size_t len, uint64_t off) {
    return st->merged ? romfs_merged_pread(st->merged, dst, len, off) : storage_base_pread(st, dst, len, off);
}
/* Mod overlay (<data-dir>/mods/overlay.json, napple build --mods): the guest parses the RomFS tables itself, so the base image is replaced by
 * a virtual one whose tables carry the overlay and whose file data points at the original image or the mod files. Any failure serves the original. */
static IpcObject *storage_with_overlay(IpcObject *o) {
    if (!o || !o->state || !g_cfg.data_dir || getenv("NAPPLE_NO_MODS")) return o;
    StorageFile *st = o->state;
    RomfsOverlay *ov = romfs_overlay_load(g_cfg.data_dir);
    if (!ov) return o;
    char why[128] = "";
    uint64_t sz = 0;
    RomfsMerged *m = romfs_merge_open(ov, storage_base_read_cb, st, st->size, &sz, why, sizeof why);
    romfs_overlay_free(ov);
    if (!m) {
        LOG("fs", "mods: romfs overlay not applied (%s); serving the original RomFS", why);
        return o;
    }
    unsigned rep, add, rem, skip;
    romfs_merged_stats(m, &rep, &add, &rem, &skip);
    st->merged = m;
    st->size = sz;
    LOG("fs", "mods: romfs overlay applied: %u replaced, %u added, %u removed, %u skipped (%llu bytes)", rep, add, rem, skip, (unsigned long long)sz);
    return o;
}
static void storage_read(IpcObject *self, IpcCall *c) {
    StorageFile *st = self->state;
    int64_t off = IPC_IN(c, int64_t, 0), size = IPC_IN(c, int64_t, 8);
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!b || off < 0 || size < 0 || (uint64_t)off > st->size || (uint64_t)size > b->size) {
        LOG("fs", "IStorage::Read rejected: offset %#llx size %#llx (storage %#llx bytes, out buffer %#llx)", (unsigned long long)off, (unsigned long long)size,
            (unsigned long long)st->size, (unsigned long long)(b ? b->size : 0));
        c->result = FS_RESULT(4002);
        return;
    }
    if ((uint64_t)off + (uint64_t)size > st->size) {
        LOG("fs", "IStorage::Read out of range: offset %#llx size %#llx (storage %#llx bytes)", (unsigned long long)off, (unsigned long long)size,
            (unsigned long long)st->size);
        c->result = FS_RESULT(4002);
        return;
    } /* out of range */
    void *dst = guest_ptr(b->addr, (uint64_t)size);
    if (!dst && size) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    if (getenv("NAPPLE_FS_TRACE") && (size >= 0x10000 || getenv("NAPPLE_FS_TRACE")[0] == 'a'))
        LOG("fs", "RomFS read offset %#llx size %#llx", (unsigned long long)off, (unsigned long long)size);
    guest_write_prepare(b->addr, (size_t)size);
    for (int64_t done = 0; done < size;) {
        ssize_t n = storage_pread(st, (char *)dst + done, (size_t)(size - done), (uint64_t)(off + done));
        if (n <= 0) {
            c->result = R_FS_UNEXPECTED;
            return;
        }
        done += n;
    }
}
static void storage_get_size(IpcObject *self, IpcCall *c) {
    StorageFile *st = self->state;
    int64_t v = (int64_t)st->size;
    IPC_OUT(c, v);
}
static void storage_write_denied(IpcObject *self, IpcCall *c) {
    (void)self;
    c->result = R_FS_INVALID_ARGUMENT;
}
static void storage_flush(IpcObject *self, IpcCall *c) {
    (void)self;
    (void)c;
}
static void storage_operate_range(IpcObject *self, IpcCall *c) {
    (void)self;
    operate_range(c, 0, NULL, NULL);
}
static const IpcCmd storage_cmds[] = {
    {0, "Read", storage_read, IPC_IMPL},        {1, "Write", storage_write_denied, IPC_IMPL},
    {2, "Flush", storage_flush, IPC_IMPL},      {3, "SetSize", storage_write_denied, IPC_IMPL},
    {4, "GetSize", storage_get_size, IPC_IMPL},
    {5, "OperateRange", storage_operate_range, IPC_IMPL},
};
static IpcObject *open_romfs_storage_path(IpcCall *c, const char *path, const char *what) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        LOG("fs", "%s: %s is missing", what, path);
        c->result = R_FS_NOT_FOUND_DATA;
        return NULL;
    }
    struct stat sb;
    fstat(fd, &sb);
    StorageFile *st = calloc(1, sizeof *st);
    st->fd = fd;
    st->src_fd[0] = st->src_fd[1] = -1;
    st->size = (uint64_t)sb.st_size;
    LOG("fs", "RomFS storage: %s (%llu bytes)", path, (unsigned long long)st->size);
    return IPC_OBJECT("IStorage", storage_cmds, st, storage_destroy);
}
/* Opens <data-dir>/romfs.delta; NULL when absent or malformed (logged). The base build dir is the sibling named by romfs.json "base_dir". */
static IpcObject *open_romfs_delta(IpcCall *c) {
    char dd[1200], p[1400];
    snprintf(dd, sizeof dd, "%s/romfs.delta", g_cfg.data_dir);
    snprintf(p, sizeof p, "%s/romfs.json", dd);
    FILE *jf = fopen(p, "r");
    if (!jf) return NULL;
    char js[4096];
    size_t jn = fread(js, 1, sizeof js - 1, jf);
    fclose(jf);
    js[jn] = 0;
    char *k = strstr(js, "\"base_dir\"");
    char base[600] = "";
    if (k && (k = strchr(k + 10, ':')) && (k = strchr(k, '"'))) {
        char *e = strchr(++k, '"');
        if (e && e - k < (long)sizeof base && !memchr(k, '/', (size_t)(e - k))) {
            memcpy(base, k, (size_t)(e - k));
            base[e - k] = 0;
        }
    }
    if (!base[0]) {
        LOG("fs", "romfs.delta: romfs.json has no usable base_dir");
        return NULL;
    }
    StorageFile *st = calloc(1, sizeof *st);
    st->src_fd[0] = st->src_fd[1] = -1;
    st->fd = -1;
    snprintf(p, sizeof p, "%s/../%s/romfs.bin", g_cfg.data_dir, base);
    st->src_fd[0] = open(p, O_RDONLY);
    if (st->src_fd[0] < 0) LOG("fs", "romfs.delta: base %s is missing", p);
    snprintf(p, sizeof p, "%s/patch.bin", dd);
    st->src_fd[1] = open(p, O_RDONLY);
    snprintf(p, sizeof p, "%s/romfs.reloc", dd);
    FILE *rf = fopen(p, "rb");
    uint8_t h[16];
    if (st->src_fd[0] >= 0 && st->src_fd[1] >= 0 && rf && fread(h, 1, 16, rf) == 16 && !memcmp(h, "RLC1", 4)) {
        uint32_t n;
        uint64_t ds;
        memcpy(&n, h + 4, 4);
        memcpy(&ds, h + 8, 8);
        st->ents = n ? calloc(n, sizeof *st->ents) : NULL;
        uint8_t r[24];
        uint32_t i = 0;
        for (; st->ents && i < n && fread(r, 1, 24, rf) == 24; i++) {
            memcpy(&st->ents[i].data_offset, r, 8);
            memcpy(&st->ents[i].source_offset, r + 8, 8);
            memcpy(&st->ents[i].source, r + 16, 4);
        }
        if (st->ents && i == n && st->ents[0].data_offset == 0) {
            st->nents = n;
            st->size = ds;
        } else {
            free(st->ents);
            st->ents = NULL;
        }
    }
    if (rf) fclose(rf);
    if (!st->ents) {
        LOG("fs", "romfs.delta: unusable (base/patch/reloc missing or malformed)");
        for (int i = 0; i < 2; i++)
            if (st->src_fd[i] >= 0) close(st->src_fd[i]);
        free(st);
        c->result = R_FS_NOT_FOUND_DATA;
        return NULL;
    }
    LOG("fs", "RomFS storage: delta layer over %s (%u extents, %llu bytes)", base, st->nents, (unsigned long long)st->size);
    return IPC_OBJECT("IStorage", storage_cmds, st, storage_destroy);
}
static IpcObject *open_romfs_storage_base(IpcCall *c) {
    char path[1200];
    snprintf(path, sizeof path, "%s/romfs.bin", g_cfg.data_dir);
    if (access(path, R_OK) != 0) {
        char d[1300];
        snprintf(d, sizeof d, "%s/romfs.delta/romfs.reloc", g_cfg.data_dir);
        if (access(d, R_OK) == 0) return open_romfs_delta(c);
    }
    if (access(path, R_OK) != 0) { /* romfs.map: development build that reads the RomFS in place from the game files */
        char d[1300];
        snprintf(d, sizeof d, "%s/romfs.map", g_cfg.data_dir);
        uint64_t sz = 0;
        RomfsMap *m = romfs_map_open(d, &sz);
        if (m) {
            StorageFile *st = calloc(1, sizeof *st);
            st->fd = -1;
            st->src_fd[0] = st->src_fd[1] = -1;
            st->map = m;
            st->size = sz;
            LOG("fs", "RomFS storage: %s (in place, %llu bytes)", d, (unsigned long long)sz);
            return IPC_OBJECT("IStorage", storage_cmds, st, storage_destroy);
        }
    }
    IpcObject *o = open_romfs_storage_path(c, path, "OpenDataStorageByCurrentProcess");
    if (!o) summary_note("romfs.bin missing");
    return o;
}
static IpcObject *open_romfs_storage(IpcCall *c) { return storage_with_overlay(open_romfs_storage_base(c)); }
/* OpenDataStorageByDataId(u8 storage id, u64 data id), commands 30 and 202: add-on content (DLC) data ids resolve to the DLC's romfs image
 * (misc.c, aoc:u); any other id is a system data archive, served when <data-dir>/sysdata/ (or <data-dir>/../sysdata/)<id as 16 hex digits>.bin exists (the console reads
 * them from the system NAND), else not found. */
/* weak default (no DLC): the host-only fs tests link fs.c without misc.c; misc.c's definition wins in the runtime. */
__attribute__((weak)) const char *aoc_data_id_romfs(uint64_t data_id) {
    (void)data_id;
    return NULL;
}
/* OpenPatchDataStorageByCurrentProcess: the romfs.bin served by OpenDataStorageByCurrentProcess already is the patched data (update layered in by the packager), so there is no separate patch storage: report that. */
static void srv_open_patch_storage(IpcObject *self, IpcCall *c) {
    (void)self;
    c->result = R_FS_NOT_FOUND_DATA;
}
static void srv_open_data_storage_by_id(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size < 16) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    uint8_t storage = IPC_IN(c, uint8_t, 0);
    uint64_t id = IPC_IN(c, uint64_t, 8);
    const char *p = aoc_data_id_romfs(id);
    char sys[1300];
    if (!p) {
        /* napple build writes them to <title dir>/sysdata, one level above the build-id data dir; a per-build copy wins. */
        snprintf(sys, sizeof sys, "%s/sysdata/%016llx.bin", g_cfg.data_dir, (unsigned long long)id);
        if (access(sys, R_OK) != 0) snprintf(sys, sizeof sys, "%s/../sysdata/%016llx.bin", g_cfg.data_dir, (unsigned long long)id);
        if (access(sys, R_OK) == 0) p = sys;
    }
    LOG("fs", "OpenDataStorageByDataId: storage %u data id %016llx%s", storage, (unsigned long long)id, p ? "" : " (no data)");
    if (!p) {
        c->result = R_FS_NOT_FOUND_DATA;
        return;
    }
    IpcObject *o = open_romfs_storage_path(c, p, "OpenDataStorageByDataId");
    if (o) ipc_out_object(c, o);
}

/* ---- fsp-srv ------------------------------------------------------------------------------------------------- */
static void srv_noop(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
}
static void srv_open_sdcard(IpcObject *s, IpcCall *c) {
    (void)s;
    char root[1200];
    snprintf(root, sizeof root, "%s/%s/sdcard", g_cfg.save_root, g_cfg.title_id);
    IpcObject *o = open_host_fs(root, 0, 0, 0);
    ((HostFs *)o->state)->sd = 1;
    ipc_out_object(c, o);
}
/* Temporary storage is erased when the application starts: the first mount in a process empties it. */
static int temporary_first_mount(const char *root) {
    static char seen[8][1300];
    static unsigned nseen;
    pthread_mutex_lock(&g_meta_mu);
    int first = 1;
    for (unsigned i = 0; i < nseen; i++)
        if (!strcmp(seen[i], root)) first = 0;
    if (first && nseen < 8) snprintf(seen[nseen++], sizeof seen[0], "%s", root);
    pthread_mutex_unlock(&g_meta_mu);
    return first;
}
/* OpenSaveDataFileSystem(u8 space, SaveDataAttribute). Opening a save that was never created creates it (the SDK's EnsureSaveData would
 * have; a console answers TargetNotFound). Account, device, system and cache saves are transactional; bcat and temporary storage are plain
 * directories, temporary storage emptied at its first mount in a process. */
static void open_save(IpcCall *c, int read_only) {
    uint8_t space = IPC_IN(c, uint8_t, 0);
    const uint8_t *attr = ipc_in(c, 8, 0x40);
    char root[1300];
    save_root(space, attr, root, sizeof root);
    uint8_t type = attr[0x20];
    if (type == SAVE_TEMPORARY && temporary_first_mount(root)) {
        rm_rf(root, 1);
        LOGV("fs", "temporary storage %s emptied (first mount)", root);
    }
    if (!read_only) mkdirs(root);
    SaveMeta m;
    int64_t quota = 0;
    pthread_mutex_lock(&g_meta_mu);
    if (meta_get(root, space, attr, &m)) quota = m.extra.data_size;
    pthread_mutex_unlock(&g_meta_mu);
    if (read_only && !path_exists(root)) {
        c->result = R_FS_TARGET_NOT_FOUND;
        return;
    }
    ipc_out_object(c, open_host_fs(root, read_only, save_type_journaled(type), quota));
}
static void srv_open_save(IpcObject *s, IpcCall *c) {
    (void)s;
    open_save(c, 0);
}
static void srv_open_save_read_only(IpcObject *s, IpcCall *c) {
    (void)s;
    open_save(c, 1);
}

/* CreateSaveDataFileSystem(SaveDataAttribute, SaveDataCreationInfo {s64 size, s64 journal size, u64 block size, u64 owner, u32 flags,
 * u8 space, ...}[, SaveDataMetaInfo][, HashSalt]): the directory and its meta record. PathAlreadyExists when it was created before (the
 * SDK's EnsureSaveData treats that as success); a directory that exists without a record (opened before it was created) is adopted. */
/* SaveDataMetaInfo {u32 size, u8 type (SaveDataMetaType: 0 None, 1 Thumbnail, 2 ExtensionContext), 11 reserved} (switchbrew): the save's meta file of
 * that type, kept next to the save root as <root>.napple-savemeta<type> and opened by OpenSaveDataMetaFile. */
static void meta_file_path(const char *root, unsigned type, char *out, size_t cap) { snprintf(out, cap, "%s.napple-savemeta%u", root, type); }
static void create_save(IpcCall *c, const uint8_t *attr, int64_t size, int64_t journal, uint64_t owner, uint32_t flags, uint8_t space, uint8_t meta_type,
                        uint32_t meta_size);
static void srv_create_save(IpcObject *s, IpcCall *c) {
    (void)s;
    const uint8_t *attr = ipc_in(c, 0, 0x40), *ci = ipc_in(c, 0x40, 0x40);
    int64_t size, journal;
    uint64_t owner;
    uint32_t flags, meta_size = 0;
    uint8_t meta_type = 0;
    memcpy(&size, ci, 8);
    memcpy(&journal, ci + 8, 8);
    memcpy(&owner, ci + 0x18, 8);
    memcpy(&flags, ci + 0x20, 4);
    if (c->in_size >= 0x90) { /* 22 and 35 carry a SaveDataMetaInfo after the creation info; 23 does not */
        memcpy(&meta_size, ipc_in(c, 0x80, 4), 4);
        meta_type = IPC_IN(c, uint8_t, 0x84);
    }
    create_save(c, attr, size, journal, owner, flags, ci[0x24], meta_type, meta_size);
}
/* 37 CreateSaveDataFileSystemWithCreationInfo2(in buffer SaveDataCreationInfo2, 0x200): switchbrew layout {u32 version, u32 reserved, SaveDataAttribute
 * 0x40 @8, s64 size @0x48, s64 journal size @0x50, s64 block size @0x58, u64 owner id @0x60, u32 flags @0x68, u8 space @0x6C, u8 format type @0x6D,
 * u8 hash salt specified @0x70, hash salt 0x20 @0x74, u8 meta type @0x94, u32 meta size @0x98}. The same save as 22 with the same meta file; the
 * hash salt only seeds the console's encryption, which the host directory does not have. */
static void srv_create_save2(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_in_buf(c, 0);
    const uint8_t *ci = b && b->size >= 0x200 ? ipc_buf_ptr(b) : NULL;
    if (!ci) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    uint8_t info[0x200];
    memcpy(info, ci, sizeof info);
    int64_t size, journal;
    uint64_t owner;
    uint32_t flags, meta_size;
    memcpy(&size, info + 0x48, 8);
    memcpy(&journal, info + 0x50, 8);
    memcpy(&owner, info + 0x60, 8);
    memcpy(&flags, info + 0x68, 4);
    memcpy(&meta_size, info + 0x98, 4);
    create_save(c, info + 8, size, journal, owner, flags, info[0x6C], info[0x94], meta_size);
}
static void create_save(IpcCall *c, const uint8_t *attr, int64_t size, int64_t journal, uint64_t owner, uint32_t flags, uint8_t space, uint8_t meta_type,
                        uint32_t meta_size) {
    if (size < 0 || journal < 0 || attr[0x20] > SAVE_CACHE || meta_type > 2) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    char root[1300];
    save_root(space, attr, root, sizeof root);
    pthread_mutex_lock(&g_meta_mu);
    SaveMeta m;
    if (meta_read(root, &m) && !m.implicit) {
        pthread_mutex_unlock(&g_meta_mu);
        c->result = R_FS_PATH_EXISTS;
        return;
    }
    mkdirs(root);
    meta_init(&m, root, space, attr); /* a record made on first use takes the creation info now */
    m.extra.owner_id = owner;
    m.extra.flags = flags;
    m.extra.data_size = size;
    m.extra.journal_size = journal;
    int w = meta_write(root, &m);
    if (!w && meta_type) {
        char mp[1400];
        meta_file_path(root, meta_type, mp, sizeof mp);
        int fd = open(mp, O_CREAT | O_TRUNC | O_RDWR, 0644);
        w = fd < 0 || ftruncate(fd, meta_size);
        if (fd >= 0) close(fd);
    }
    pthread_mutex_unlock(&g_meta_mu);
    if (w) {
        c->result = errno_result();
        return;
    }
    LOGV("fs", "created save %s (id %#llx, size %#llx, journal %#llx)", root, (unsigned long long)m.id, (unsigned long long)size, (unsigned long long)journal);
}
static void srv_delete_save_by_id(IpcObject *s, IpcCall *c, int space, uint64_t id) {
    (void)s;
    char root[1300];
    SaveMeta m;
    pthread_mutex_lock(&g_meta_mu);
    int found = find_by_id(space, id, root, sizeof root, &m);
    if (found) save_delete(root);
    pthread_mutex_unlock(&g_meta_mu);
    if (!found) c->result = R_FS_TARGET_NOT_FOUND;
}
static void srv_delete_save(IpcObject *s, IpcCall *c) { srv_delete_save_by_id(s, c, -1, IPC_IN(c, uint64_t, 0)); }
static void srv_delete_save_by_space(IpcObject *s, IpcCall *c) { srv_delete_save_by_id(s, c, IPC_IN(c, uint8_t, 0), IPC_IN(c, uint64_t, 8)); }
/* (u8 space, SaveDataAttribute) or (SaveDataAttribute, u8 space): the SDK versions differ in the order. A program id has a nonzero top
 * byte (0x01...), the padding after a leading space id is zero. */
static void space_and_attr(const IpcCall *c, uint8_t *space, const uint8_t **attr) {
    const uint8_t *in = ipc_in(c, 0, 0x48);
    int lead = 1;
    for (int i = 1; i < 8; i++)
        if (in[i]) lead = 0;
    if (lead) {
        *space = in[0];
        *attr = in + 8;
    } else {
        *attr = in;
        *space = in[0x40];
    }
}
static void srv_delete_save_by_attr(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t space;
    const uint8_t *attr;
    space_and_attr(c, &space, &attr);
    char root[1300];
    save_root(space, attr, root, sizeof root);
    pthread_mutex_lock(&g_meta_mu);
    SaveMeta m;
    int found = meta_get(root, space, attr, &m);
    if (found) save_delete(root);
    pthread_mutex_unlock(&g_meta_mu);
    if (!found) c->result = R_FS_TARGET_NOT_FOUND;
}
/* ExtendSaveDataFileSystem(u8 space, u64 id, s64 size, s64 journal size): sizes only grow. */
static void srv_extend_save(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t space = IPC_IN(c, uint8_t, 0);
    uint64_t id = IPC_IN(c, uint64_t, 8);
    int64_t size = IPC_IN(c, int64_t, 16), journal = IPC_IN(c, int64_t, 24);
    char root[1300];
    SaveMeta m;
    pthread_mutex_lock(&g_meta_mu);
    if (!find_by_id(space, id, root, sizeof root, &m)) c->result = R_FS_TARGET_NOT_FOUND;
    else if (size < m.extra.data_size || journal < m.extra.journal_size) c->result = R_FS_INVALID_ARGUMENT;
    else {
        m.extra.data_size = size;
        m.extra.journal_size = journal;
        if (meta_write(root, &m)) c->result = errno_result();
    }
    pthread_mutex_unlock(&g_meta_mu);
}
/* Cache storage of this application by index (DeleteCacheStorage / GetCacheStorageSize): space 0 (user), type cache, no user id. */
static void cache_attr(uint16_t index, uint8_t attr[0x40]) {
    memset(attr, 0, 0x40);
    memcpy(attr, &g_program_id, 8);
    attr[0x20] = SAVE_CACHE;
    memcpy(attr + 0x22, &index, 2);
}
static void srv_delete_cache(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t attr[0x40];
    cache_attr(IPC_IN(c, uint16_t, 0), attr);
    char root[1300];
    save_root(0, attr, root, sizeof root);
    pthread_mutex_lock(&g_meta_mu);
    SaveMeta m;
    int found = meta_get(root, 0, attr, &m);
    if (found) save_delete(root);
    pthread_mutex_unlock(&g_meta_mu);
    if (!found) c->result = R_FS_TARGET_NOT_FOUND;
}
static void srv_cache_size(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t attr[0x40];
    cache_attr(IPC_IN(c, uint16_t, 0), attr);
    char root[1300];
    save_root(0, attr, root, sizeof root);
    pthread_mutex_lock(&g_meta_mu);
    SaveMeta m;
    int found = meta_get(root, 0, attr, &m);
    pthread_mutex_unlock(&g_meta_mu);
    if (!found) {
        c->result = R_FS_TARGET_NOT_FOUND;
        return;
    }
    IPC_OUT(c, m.extra.data_size);
    IPC_OUT(c, m.extra.journal_size);
}

/* Extra data reads and (masked) writes. */
static void extra_out(IpcCall *c, const SaveMeta *m, const uint8_t *mask) {
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!b) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    uint8_t out[0x200];
    memcpy(out, &m->extra, sizeof out);
    if (mask)
        for (unsigned i = 0; i < sizeof out; i++) out[i] &= mask[i];
    ipc_write_buf(c, b, out, sizeof out);
}
static const uint8_t *in_extra(const IpcCall *c, unsigned n) {
    const IpcBuf *b = ipc_in_buf(c, n);
    return b && b->size >= 0x200 ? ipc_buf_ptr(b) : NULL;
}
static void extra_write(IpcCall *c, const char *root, SaveMeta *m, const uint8_t *data, const uint8_t *mask) {
    if (!data) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    uint8_t *cur = (uint8_t *)&m->extra;
    for (unsigned i = 0; i < 0x200; i++) cur[i] = mask ? (uint8_t)((cur[i] & ~mask[i]) | (data[i] & mask[i])) : data[i];
    if (meta_write(root, m)) c->result = errno_result();
}
static void srv_read_extra_by_space(IpcObject *s, IpcCall *c) {
    (void)s;
    char root[1300];
    SaveMeta m;
    pthread_mutex_lock(&g_meta_mu);
    int found = find_by_id(IPC_IN(c, uint8_t, 0), IPC_IN(c, uint64_t, 8), root, sizeof root, &m);
    pthread_mutex_unlock(&g_meta_mu);
    if (!found) {
        c->result = R_FS_TARGET_NOT_FOUND;
        return;
    }
    extra_out(c, &m, NULL);
}
static void srv_read_extra(IpcObject *s, IpcCall *c) {
    (void)s;
    char root[1300];
    SaveMeta m;
    pthread_mutex_lock(&g_meta_mu);
    int found = find_by_id(-1, IPC_IN(c, uint64_t, 0), root, sizeof root, &m);
    pthread_mutex_unlock(&g_meta_mu);
    if (!found) {
        c->result = R_FS_TARGET_NOT_FOUND;
        return;
    }
    extra_out(c, &m, NULL);
}
/* WriteSaveDataFileSystemExtraData(WithMask)(u64 id, u8 space, buffer[, mask]) */
static void write_extra_by_id(IpcCall *c, int masked) {
    char root[1300];
    SaveMeta m;
    pthread_mutex_lock(&g_meta_mu);
    if (!find_by_id(IPC_IN(c, uint8_t, 8), IPC_IN(c, uint64_t, 0), root, sizeof root, &m)) c->result = R_FS_TARGET_NOT_FOUND;
    else extra_write(c, root, &m, in_extra(c, 0), masked ? in_extra(c, 1) : NULL);
    pthread_mutex_unlock(&g_meta_mu);
}
static void srv_write_extra(IpcObject *s, IpcCall *c) {
    (void)s;
    write_extra_by_id(c, 0);
}
static void srv_write_extra_mask(IpcObject *s, IpcCall *c) {
    (void)s;
    write_extra_by_id(c, 1);
}
static int meta_by_attr(IpcCall *c, char *root, size_t cap, SaveMeta *m) {
    uint8_t space;
    const uint8_t *attr;
    space_and_attr(c, &space, &attr);
    save_root(space, attr, root, cap);
    pthread_mutex_lock(&g_meta_mu);
    int found = meta_get(root, space, attr, m);
    pthread_mutex_unlock(&g_meta_mu);
    if (!found) c->result = R_FS_TARGET_NOT_FOUND;
    return found;
}
static void srv_read_extra_by_attr(IpcObject *s, IpcCall *c) {
    (void)s;
    char root[1300];
    SaveMeta m;
    if (meta_by_attr(c, root, sizeof root, &m)) extra_out(c, &m, NULL);
}
static void srv_read_extra_mask_by_attr(IpcObject *s, IpcCall *c) {
    (void)s;
    char root[1300];
    SaveMeta m;
    const uint8_t *mask = in_extra(c, 0);
    if (!mask) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    if (meta_by_attr(c, root, sizeof root, &m)) extra_out(c, &m, mask);
}
static void srv_write_extra_mask_by_attr(IpcObject *s, IpcCall *c) {
    (void)s;
    char root[1300];
    SaveMeta m;
    if (!meta_by_attr(c, root, sizeof root, &m)) return;
    pthread_mutex_lock(&g_meta_mu);
    extra_write(c, root, &m, in_extra(c, 0), in_extra(c, 1));
    pthread_mutex_unlock(&g_meta_mu);
}

/* ISaveDataInfoReader: SaveDataInfo {u64 id, u8 space, u8 type, pad[6], u128 user, u64 system id, u64 program id, s64 size, u16 index,
 * u8 rank, pad} (0x60 each), over the saves that have a meta record. */
typedef struct {
    uint8_t (*infos)[0x60];
    unsigned n, cap, next;
    int space, only_cache;
    uint8_t filter[0x48]; /* SaveDataFilter: five "compare this field" flags, option, reserved, then the SaveDataAttribute to compare against */
    int use_filter;
} InfoReader;
static int info_collect(const char *root, const SaveMeta *m, void *arg) {
    (void)root;
    InfoReader *r = arg;
    const uint8_t *a = m->extra.attr;
    if ((r->space >= 0 && m->space != r->space) || (r->only_cache && a[0x20] != SAVE_CACHE)) return 0;
    if (r->use_filter) {
        const uint8_t *f = r->filter, *fa = f + 8;
        if ((f[0] && memcmp(a, fa, 8)) || (f[1] && a[0x20] != fa[0x20]) || (f[2] && memcmp(a + 8, fa + 8, 16)) || (f[3] && memcmp(a + 0x18, fa + 0x18, 8)) ||
            (f[4] && memcmp(a + 0x22, fa + 0x22, 2)))
            return 0;
    }
    if (r->n == r->cap) {
        unsigned nc = r->cap ? r->cap * 2 : 16;
        void *p = realloc(r->infos, (size_t)nc * 0x60);
        if (!p) return 1;
        r->infos = p;
        r->cap = nc;
    }
    uint8_t *e = r->infos[r->n++];
    memset(e, 0, 0x60);
    memcpy(e, &m->id, 8);
    e[8] = m->space;
    e[9] = a[0x20];
    memcpy(e + 0x10, a + 8, 16);
    memcpy(e + 0x20, a + 0x18, 8);
    memcpy(e + 0x28, a, 8);
    memcpy(e + 0x30, &m->extra.data_size, 8);
    memcpy(e + 0x38, a + 0x22, 2);
    e[0x3a] = a[0x21];
    return 0;
}
static void reader_destroy(IpcObject *o) {
    InfoReader *r = o->state;
    if (r) {
        free(r->infos);
        free(r);
    }
}
static void reader_read(IpcObject *self, IpcCall *c) {
    InfoReader *r = self->state;
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!b) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    int64_t n = 0;
    while (r->next < r->n && (uint64_t)(n + 1) * 0x60 <= b->size) {
        IpcBuf at = *b;
        at.addr = b->addr + (uint64_t)n * 0x60;
        at.size = 0x60;
        ipc_write_buf(c, &at, r->infos[r->next++], 0x60);
        n++;
    }
    IPC_OUT(c, n);
}
static const IpcCmd reader_cmds[] = {{0, "ReadSaveDataInfo", reader_read, IPC_IMPL}};
static InfoReader *collect_infos(int space, int only_cache, const uint8_t *filter) {
    InfoReader *r = calloc(1, sizeof *r);
    r->space = space;
    r->only_cache = only_cache;
    if (filter) {
        memcpy(r->filter, filter, sizeof r->filter);
        r->use_filter = 1;
    }
    pthread_mutex_lock(&g_meta_mu);
    meta_walk(host_save_dir(), 6, info_collect, r);
    pthread_mutex_unlock(&g_meta_mu);
    return r;
}
static void open_reader(IpcCall *c, int space, int only_cache) {
    ipc_out_object(c, IPC_OBJECT("ISaveDataInfoReader", reader_cmds, collect_infos(space, only_cache, NULL), reader_destroy));
}
static void srv_open_reader(IpcObject *s, IpcCall *c) {
    (void)s;
    open_reader(c, -1, 0);
}
static void srv_open_reader_space(IpcObject *s, IpcCall *c) {
    (void)s;
    open_reader(c, IPC_IN(c, uint8_t, 0), 0);
}
static void srv_open_reader_cache(IpcObject *s, IpcCall *c) {
    (void)s;
    open_reader(c, 0, 1);
}
static void srv_open_data_storage(IpcObject *s, IpcCall *c) {
    (void)s;
    IpcObject *o = open_romfs_storage(c);
    if (o) ipc_out_object(c, o);
}
/* ---- RomFS as an IFileSystem ------------------------------------------------------------------------------------
 * OpenDataFileSystemByCurrentProcess / ByProgramId and OpenFileSystemWithId(Rom, Data) hand out the RomFS image as a read-only file system: the
 * directory and file tables are parsed on the host (romfs_read.c) and the files are extents of the same storage the IStorage commands serve
 * (romfs.bin, romfs.map, the BKTR delta layer or the mod overlay), so both views of a title's RomFS always agree. */
typedef struct {
    StorageFile *st;
    RomfsImg *img;
    unsigned refs;
} RomView;
static ssize_t romview_read_cb(void *ctx, void *dst, size_t len, uint64_t off) { return storage_pread(((RomView *)ctx)->st, dst, len, off); }
static void romview_unref(RomView *v) {
    if (!v || __atomic_sub_fetch(&v->refs, 1, __ATOMIC_ACQ_REL)) return;
    romfs_img_close(v->img);
    IpcObject tmp = {.state = v->st};
    storage_destroy(&tmp);
    free(v);
}
/* Takes the StorageFile out of a freshly made IStorage object (nothing else holds a reference to it yet). */
static StorageFile *take_storage(IpcObject *o) {
    if (!o) return NULL;
    StorageFile *st = o->state;
    o->state = NULL;
    o->destroy = NULL;
    free(o);
    return st;
}
static RomView *romview_from_storage(IpcObject *o) {
    StorageFile *st = take_storage(o);
    if (!st) return NULL;
    RomView *v = calloc(1, sizeof *v);
    v->st = st;
    v->refs = 1;
    v->img = romfs_img_open(romview_read_cb, v, st->size);
    if (!v->img) {
        LOG("fs", "RomFS file system: the image is not a parsable RomFS");
        romview_unref(v);
        return NULL;
    }
    return v;
}
typedef struct {
    RomView *v;
    uint64_t off, size;
} RomFile;
static void romfile_destroy(IpcObject *o) {
    RomFile *f = o->state;
    if (f) {
        romview_unref(f->v);
        free(f);
    }
}
static void romfile_read(IpcObject *self, IpcCall *c) {
    RomFile *f = self->state;
    int64_t off = IPC_IN(c, int64_t, 8), size = IPC_IN(c, int64_t, 16);
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!b || off < 0 || size < 0) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    if ((uint64_t)size > b->size) size = (int64_t)b->size;
    void *dst = guest_ptr(b->addr, (uint64_t)size);
    if (!dst && size) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    guest_write_prepare(b->addr, (size_t)size);
    uint64_t got = 0;
    while (got < (uint64_t)size) {
        ssize_t n = romfs_img_read(f->v->img, f->off, f->size, (uint64_t)off + got, (char *)dst + got, (size_t)((uint64_t)size - got));
        if (n < 0) {
            c->result = R_FS_UNEXPECTED;
            return;
        }
        if (n == 0) break;
        got += (uint64_t)n;
    }
    IPC_OUT(c, got);
}
static void romfile_get_size(IpcObject *self, IpcCall *c) {
    RomFile *f = self->state;
    int64_t v = (int64_t)f->size;
    IPC_OUT(c, v);
}
static void read_only_denied(IpcObject *self, IpcCall *c) {
    (void)self;
    c->result = R_FS_INVALID_ARGUMENT;
}
static void romfile_operate_range(IpcObject *self, IpcCall *c) {
    (void)self;
    operate_range(c, 0, NULL, NULL);
}
static const IpcCmd romfile_cmds[] = {
    {0, "Read", romfile_read, IPC_IMPL},         {1, "Write", read_only_denied, IPC_IMPL},    {2, "Flush", srv_noop, IPC_IMPL},
    {3, "SetSize", read_only_denied, IPC_IMPL},  {4, "GetSize", romfile_get_size, IPC_IMPL},  {5, "OperateRange", romfile_operate_range, IPC_IMPL},
    {6, "OperateRangeWithBuffer", romfile_operate_range, IPC_IMPL},
};
typedef struct {
    RomView *v;
    char path[0x302];
    unsigned next;
    int mode;
} RomDir;
static void romdir_destroy(IpcObject *o) {
    RomDir *d = o->state;
    if (d) {
        romview_unref(d->v);
        free(d);
    }
}
static int romdir_wanted(const RomDir *d, int is_dir) { return is_dir ? (d->mode & 1) : (d->mode & 2); }
static void romdir_read(IpcObject *self, IpcCall *c) {
    RomDir *d = self->state;
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!b) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    uint8_t *out = guest_ptr(b->addr, b->size);
    if (!out) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    int64_t max = (int64_t)(b->size / 0x310), n = 0;
    guest_write_prepare(b->addr, (size_t)b->size);
    char name[0x301];
    int isdir;
    uint64_t sz;
    while (n < max && romfs_img_child(d->v->img, d->path, d->next, name, sizeof name, &isdir, &sz)) {
        d->next++;
        if (!romdir_wanted(d, isdir)) continue;
        uint8_t *ent = out + n * 0x310;
        memset(ent, 0, 0x310);
        memcpy(ent, name, strnlen(name, 0x300));
        ent[0x304] = isdir ? 0 : 1;
        int64_t s = (int64_t)sz;
        memcpy(ent + 0x308, &s, 8);
        n++;
    }
    IPC_OUT(c, n);
}
static void romdir_entry_count(IpcObject *self, IpcCall *c) {
    RomDir *d = self->state;
    int64_t n = 0;
    char name[0x301];
    int isdir;
    for (unsigned i = 0; romfs_img_child(d->v->img, d->path, i, name, sizeof name, &isdir, NULL); i++) n += romdir_wanted(d, isdir) != 0;
    IPC_OUT(c, n);
}
static const IpcCmd romdir_cmds[] = {{0, "Read", romdir_read, IPC_IMPL}, {1, "GetEntryCount", romdir_entry_count, IPC_IMPL}};
/* The RomFS path of a call's buffer argument ("/a/b"), validated like the host file systems' paths. */
static int rom_path(IpcCall *c, unsigned n, char *out, size_t cap) {
    static const HostFs none = {.root = ""};
    char gp[0x302];
    return guest_path(c, n, gp, sizeof gp) && host_path(&none, gp, out, cap);
}
static void romfs_get_entry_type(IpcObject *s, IpcCall *c) {
    RomView *v = s->state;
    char p[0x302];
    if (!rom_path(c, 0, p, sizeof p)) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    int k = romfs_img_lookup(v->img, p, NULL, NULL);
    if (k == ROMFS_NONE) {
        c->result = R_FS_PATH_NOT_FOUND;
        return;
    }
    uint32_t t = k == ROMFS_DIR ? 0 : 1;
    IPC_OUT(c, t);
}
static void romfs_open_file(IpcObject *s, IpcCall *c) {
    RomView *v = s->state;
    char p[0x302];
    if (!rom_path(c, 0, p, sizeof p)) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    if (IPC_IN(c, uint32_t, 0) & 6) { /* write or append: this file system is read-only */
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    uint64_t off, size;
    int k = romfs_img_lookup(v->img, p, &off, &size);
    if (k != ROMFS_FILE) {
        c->result = R_FS_PATH_NOT_FOUND;
        return;
    }
    RomFile *f = calloc(1, sizeof *f);
    f->v = v;
    f->off = off;
    f->size = size;
    __atomic_add_fetch(&v->refs, 1, __ATOMIC_RELAXED);
    ipc_out_object(c, IPC_OBJECT("IFile", romfile_cmds, f, romfile_destroy));
}
static void romfs_open_directory(IpcObject *s, IpcCall *c) {
    RomView *v = s->state;
    char p[0x302];
    if (!rom_path(c, 0, p, sizeof p)) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    if (romfs_img_lookup(v->img, p, NULL, NULL) != ROMFS_DIR) {
        c->result = R_FS_PATH_NOT_FOUND;
        return;
    }
    RomDir *d = calloc(1, sizeof *d);
    d->v = v;
    d->mode = (int)IPC_IN(c, uint32_t, 0);
    snprintf(d->path, sizeof d->path, "%s", p);
    __atomic_add_fetch(&v->refs, 1, __ATOMIC_RELAXED);
    ipc_out_object(c, IPC_OBJECT("IDirectory", romdir_cmds, d, romdir_destroy));
}
static void romfs_total_space(IpcObject *s, IpcCall *c) {
    RomView *v = s->state;
    int64_t t = (int64_t)v->st->size;
    IPC_OUT(c, t);
}
static void romfs_free_space(IpcObject *s, IpcCall *c) {
    (void)s;
    int64_t t = 0; /* nothing can be written to it */
    IPC_OUT(c, t);
}
static void romfs_destroy(IpcObject *o) { romview_unref(o->state); }
static const IpcCmd romfs_fs_cmds[] = {
    {0, "CreateFile", read_only_denied, IPC_IMPL},
    {1, "DeleteFile", read_only_denied, IPC_IMPL},
    {2, "CreateDirectory", read_only_denied, IPC_IMPL},
    {3, "DeleteDirectory", read_only_denied, IPC_IMPL},
    {4, "DeleteDirectoryRecursively", read_only_denied, IPC_IMPL},
    {5, "RenameFile", read_only_denied, IPC_IMPL},
    {6, "RenameDirectory", read_only_denied, IPC_IMPL},
    {7, "GetEntryType", romfs_get_entry_type, IPC_IMPL},
    {8, "OpenFile", romfs_open_file, IPC_IMPL},
    {9, "OpenDirectory", romfs_open_directory, IPC_IMPL},
    {10, "Commit", srv_noop, IPC_IMPL},
    {11, "GetFreeSpaceSize", romfs_free_space, IPC_IMPL},
    {12, "GetTotalSpaceSize", romfs_total_space, IPC_IMPL},
    {13, "CleanDirectoryRecursively", read_only_denied, IPC_IMPL},
};

/* ---- fsp-srv: content file systems -------------------------------------------------------------------------------- */
/* The data storage behind a program id: the running application and its update title are the current RomFS, anything else resolves like a data id (a DLC's
 * romfs image, or a firmware data archive in sysdata/). NULL with c->result set when there is none. */
static IpcObject *open_storage_for_program(IpcCall *c, uint64_t id, const char *what) {
    if (id == g_program_id || id == (g_program_id | 0x800)) return open_romfs_storage(c);
    const char *p = aoc_data_id_romfs(id);
    char sys[1300];
    if (!p) {
        snprintf(sys, sizeof sys, "%s/sysdata/%016llx.bin", g_cfg.data_dir, (unsigned long long)id);
        if (access(sys, R_OK) != 0) snprintf(sys, sizeof sys, "%s/../sysdata/%016llx.bin", g_cfg.data_dir, (unsigned long long)id);
        if (access(sys, R_OK) == 0) p = sys;
    }
    LOG("fs", "%s: program id %016llx%s", what, (unsigned long long)id, p ? "" : " (no data)");
    if (!p) {
        c->result = R_FS_NOT_FOUND_DATA;
        return NULL;
    }
    return open_romfs_storage_path(c, p, what);
}
static void out_romfs_fs(IpcCall *c, IpcObject *storage) {
    if (!storage) {
        if (!c->result) c->result = R_FS_NOT_FOUND_DATA;
        return;
    }
    RomView *v = romview_from_storage(storage);
    if (!v) {
        c->result = R_FS_NOT_FOUND_DATA;
        return;
    }
    ipc_out_object(c, IPC_OBJECT("IFileSystem", romfs_fs_cmds, v, romfs_destroy));
}
/* A read-only view of a directory of the data directory (the HostFs checks every path, so nothing outside `root` is reachable). */
static IpcObject *open_content_dir_fs(const char *root) {
    HostFs *fs = calloc(1, sizeof *fs);
    snprintf(fs->root, sizeof fs->root, "%s", root);
    fs->read_only = 1;
    LOG("fs", "mounted content directory %s (read only)", root);
    return IPC_OBJECT("IFileSystem", fs_cmds, fs, NULL);
}
/* Manual (the title's offline HTML documents): <data-dir>/html, written by `napple build` from the title's HtmlDocument content. */
static IpcObject *open_manual_fs(IpcCall *c) {
    char p[1300];
    struct stat sb;
    snprintf(p, sizeof p, "%s/html", g_cfg.data_dir ? g_cfg.data_dir : ".");
    if (stat(p, &sb) || !S_ISDIR(sb.st_mode)) {
        c->result = R_FS_NOT_FOUND_DATA;
        return NULL;
    }
    return open_content_dir_fs(p);
}
/* Control: control.nacp of the data directory, linked into <save-root>/<title>/content/control (the file system is rooted there so the module files
 * next to control.nacp stay out of reach). */
static IpcObject *open_control_fs(IpcCall *c) {
    char src[1300], dir[1300], link[1400];
    snprintf(src, sizeof src, "%s/control.nacp", g_cfg.data_dir ? g_cfg.data_dir : ".");
    if (access(src, R_OK) != 0) {
        c->result = R_FS_NOT_FOUND_DATA;
        return NULL;
    }
    snprintf(dir, sizeof dir, "%s/%s/content/control", g_cfg.save_root, g_cfg.title_id);
    mkdirs(dir);
    snprintf(link, sizeof link, "%s/control.nacp", dir);
    char abs[PATH_MAX];
    if (realpath(src, abs)) {
        unlink(link);
        if (symlink(abs, link)) LOG("fs", "control file system: cannot link %s: %s", abs, strerror(errno));
    }
    return open_content_dir_fs(dir);
}
/* FileSystemProxyType: 0 Code, 1 Rom, 2 Logo, 3 Control, 4 Manual, 5 Meta, 6 Data, 7 Package. The host has no NCA files, so only the types it can serve exist:
 * Rom and Data (the RomFS of the program id), Control, Manual. The others are "path not found", which is what a console answers for content that was never
 * installed. */
static void open_content_fs(IpcCall *c, uint32_t type, uint64_t program_id) {
    IpcObject *fsobj = NULL;
    switch (type) {
        case 1:
        case 6: out_romfs_fs(c, open_storage_for_program(c, program_id, "OpenFileSystemWithId")); return;
        case 3: fsobj = open_control_fs(c); break;
        case 4: fsobj = open_manual_fs(c); break;
        default: c->result = R_FS_PATH_NOT_FOUND; return;
    }
    if (fsobj) ipc_out_object(c, fsobj);
}
/* 10 OpenFileSystemWithId(buffer FspPath; u8 content attributes, u32 type, u64 program id) */
static void srv_open_fs_with_id(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size < 16) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    open_content_fs(c, IPC_IN(c, uint32_t, 4), IPC_IN(c, uint64_t, 8));
}
/* 7 OpenFileSystemWithPatch(u32 type, u64 program id): only Manual is allowed (the patched manual); the host has one manual. */
static void srv_open_fs_with_patch(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size < 16 || IPC_IN(c, uint32_t, 0) != 4) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    open_content_fs(c, 4, IPC_IN(c, uint64_t, 8));
}
static void srv_open_data_fs_current(IpcObject *s, IpcCall *c) {
    (void)s;
    out_romfs_fs(c, open_romfs_storage(c));
}
static void srv_open_data_fs_by_program(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size < 8) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    out_romfs_fs(c, open_storage_for_program(c, IPC_IN(c, uint64_t, 0), "OpenDataFileSystemByProgramId"));
}
static void srv_open_data_storage_by_program(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size < 8) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    IpcObject *o = open_storage_for_program(c, IPC_IN(c, uint64_t, 0), "OpenDataStorageByProgramId");
    if (o) ipc_out_object(c, o);
}
/* 205 OpenDataStorageWithProgramIndex(u8 index): an application has one program (index 0); another index is "not found". */
static void srv_open_data_storage_with_index(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size < 1 || IPC_IN(c, uint8_t, 0) != 0) {
        c->result = R_FS_NOT_FOUND_DATA;
        return;
    }
    IpcObject *o = open_romfs_storage(c);
    if (o) ipc_out_object(c, o);
}
/* 100 OpenImageDirectoryFileSystem(u32 id: 0 NAND, 1 SD card) and 130 OpenCustomStorageFileSystem(u32 id: 0 user, 1 SD card): writable directories below the
 * title's save root. */
static void srv_open_image_directory(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t id = IPC_IN(c, uint32_t, 0);
    if (id > 1) {
        c->result = R_FS_INVALID_ENUM;
        return;
    }
    char root[1300];
    snprintf(root, sizeof root, "%s/%s/image/%s", g_cfg.save_root, g_cfg.title_id, id ? "sd" : "nand");
    ipc_out_object(c, open_host_fs(root, 0, 0, 0));
}
static void srv_open_custom_storage(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t id = IPC_IN(c, uint32_t, 0);
    if (id > 1) {
        c->result = R_FS_INVALID_ENUM;
        return;
    }
    char root[1300];
    snprintf(root, sizeof root, "%s/%s/custom/%s", g_cfg.save_root, g_cfg.title_id, id ? "sd" : "user");
    ipc_out_object(c, open_host_fs(root, 0, 0, 0));
}

/* 510 OpenSystemDataUpdateEventNotifier -> IEventNotifier {0 GetEventHandle -> event}: the event is signalled by 511 NotifySystemDataUpdateEvent (the system data
 * archives are read-only here, so only an explicit notify fires it). */
static uint32_t g_sysdata_events[8];
static unsigned g_sysdata_nevents;
static pthread_mutex_t g_sysdata_mu = PTHREAD_MUTEX_INITIALIZER;
static void notifier_get_event(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t r = 0, w = event_create(&r);
    pthread_mutex_lock(&g_sysdata_mu);
    if (g_sysdata_nevents < 8) g_sysdata_events[g_sysdata_nevents++] = w;
    pthread_mutex_unlock(&g_sysdata_mu);
    ipc_out_copy_handle(c, r);
}
static const IpcCmd notifier_cmds[] = {{0, "GetEventHandle", notifier_get_event, IPC_IMPL}};
static void srv_open_sysdata_notifier(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IEventNotifier", notifier_cmds, NULL, NULL));
}
static void srv_notify_sysdata_update(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    pthread_mutex_lock(&g_sysdata_mu);
    for (unsigned i = 0; i < g_sysdata_nevents; i++) event_signal_handle(g_sysdata_events[i]);
    pthread_mutex_unlock(&g_sysdata_mu);
}

/* ---- fsp-srv: state the SDK reads back ----------------------------------------------------------------------------- */
static int g_sd_accessible = 1;
static uint8_t g_sd_seed[16];
static uint32_t g_access_log_mode;
/* 27 IsExFatSupported: the exFAT driver ships with the firmware since 2.0.0 and the host file system under the SD directory has no FAT limits. */
static void srv_is_exfat_supported(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = 1;
    IPC_OUT(c, v);
}
static void srv_set_sd_accessibility(IpcObject *s, IpcCall *c) {
    (void)s;
    g_sd_accessible = c->in_size >= 1 && IPC_IN(c, uint8_t, 0) != 0;
}
/* 631 IsSdCardAccessible: the SD card is a directory below the save root; it is accessible while the guest has not turned it off and the directory can be used. */
static void srv_is_sd_accessible(IpcObject *s, IpcCall *c) {
    (void)s;
    char root[1300];
    snprintf(root, sizeof root, "%s/%s/sdcard", g_cfg.save_root, g_cfg.title_id);
    mkdirs(root);
    uint8_t v = g_sd_accessible && access(root, W_OK | X_OK) == 0;
    IPC_OUT(c, v);
}
static void srv_set_sd_encryption_seed(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size < 16) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    memcpy(g_sd_seed, ipc_in(c, 0, 16), 16);
}
static void srv_set_log_mode(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size >= 4) g_access_log_mode = IPC_IN(c, uint32_t, 0);
}
static void srv_get_log_mode(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v = g_access_log_mode;
    IPC_OUT(c, v);
}
/* 1011 GetProgramIndexForAccessLog -> {u32 version, u32 program index}: version 1 of the access log format, program 0 of the application. */
static void srv_get_program_index_for_access_log(IpcObject *s, IpcCall *c) {
    (void)s;
    uint32_t v[2] = {1, 0};
    ipc_out(c, v, sizeof v);
}
/* 612 GetFreeSpaceSizeForSaveData(u8 space id) -> s64: the free space of the volume that holds the save directory. */
static void srv_free_space_for_save(IpcObject *s, IpcCall *c) {
    (void)s;
    struct statvfs sv;
    int64_t v = 0;
    if (statvfs(host_save_dir(), &sv) == 0) v = (int64_t)((uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize);
    IPC_OUT(c, v);
}
/* 616 GetSaveDataCommitId(u8 space id, u64 save data id) -> u64: the commit id of the save's extra data, advanced by every commit (fs_commit). */
static void srv_get_commit_id(IpcObject *s, IpcCall *c) {
    (void)s;
    char root[1300];
    SaveMeta m;
    pthread_mutex_lock(&g_meta_mu);
    int found = find_by_id(IPC_IN(c, uint8_t, 0), IPC_IN(c, uint64_t, 8), root, sizeof root, &m);
    pthread_mutex_unlock(&g_meta_mu);
    if (!found) {
        c->result = R_FS_TARGET_NOT_FOUND;
        return;
    }
    uint64_t id = (uint64_t)m.extra.commit_id;
    IPC_OUT(c, id);
}
/* 64 OpenSaveDataInternalStorageFileSystem(u8 space id, u64 save data id): the save's own backing directory, writable like the save itself. */
static void srv_open_internal_storage(IpcObject *s, IpcCall *c) {
    (void)s;
    char root[1300];
    SaveMeta m;
    pthread_mutex_lock(&g_meta_mu);
    int found = find_by_id(IPC_IN(c, uint8_t, 0), IPC_IN(c, uint64_t, 8), root, sizeof root, &m);
    pthread_mutex_unlock(&g_meta_mu);
    if (!found) {
        c->result = R_FS_TARGET_NOT_FOUND;
        return;
    }
    ipc_out_object(c, open_host_fs(root, 0, save_type_journaled(m.extra.attr[0x20]), m.extra.data_size));
}
/* 67 FindSaveDataWithFilter(u8 space id, SaveDataFilter) -> s64 count + SaveDataInfo[1]; 68 OpenSaveDataInfoReaderWithFilter(u8 space id, SaveDataFilter). */
static void srv_find_with_filter(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size < 0x50) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    InfoReader *r = collect_infos(IPC_IN(c, uint8_t, 0), 0, ipc_in(c, 8, 0x48));
    int64_t n = 0;
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (r->n && b && b->size >= 0x60) {
        ipc_write_buf(c, b, r->infos[r->next++], 0x60);
        n = 1;
    }
    free(r->infos);
    free(r);
    IPC_OUT(c, n);
}
static void srv_open_reader_filter(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size < 0x50) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    ipc_out_object(c, IPC_OBJECT("ISaveDataInfoReader", reader_cmds, collect_infos(IPC_IN(c, uint8_t, 0), 0, ipc_in(c, 8, 0x48)), reader_destroy));
}

/* ---- fsp-srv: access-checked commands (ipc-depth-95 r10) -----------------------------------------------------------------
 * fsp-srv checks every command against the FS permission bits of the calling process (main.npdm, ACI0 FsAccessHeader; switchbrew 'NPDM' FsAccessFlag
 * table). The masks below are the switchbrew 'Filesystem services' permission tables: a command is allowed when the process has at least one bit of the
 * mask. Bits 62 (Debug) and 63 (FullPermission) count only on a console in debug mode, which this one is not. A retail application holds none of the
 * transfer / content-manager / rights-id bits, so for it these commands answer PermissionDenied, exactly as on the console. Without a usable main.npdm
 * the runtime checks nothing (like the service and svc access lists), and the commands take their permitted path. */
#define FS_PERM_DEBUG_BITS (3ull << 62)
static int fs_permitted(uint64_t mask) { return !g_npdm.valid || (g_npdm.fs_perms & mask & ~FS_PERM_DEBUG_BITS) != 0; }
#define FS_REQUIRE(mask)                                                                                                                                       \
    if (!fs_permitted(mask)) {                                                                                                                                 \
        c->result = R_FS_PERMISSION_DENIED;                                                                                                                    \
        return;                                                                                                                                                \
    }
#define FS_PERM_MOUNT_GAME_CARD 0x8000000000000010ull         /* GameCard */
#define FS_PERM_OPEN_SAVE_META_FILE 0x8000000000020000ull     /* SaveDataMeta */
#define FS_PERM_TRANSFER_MANAGER 0x8000000040000000ull        /* SaveDataTransfer */
#define FS_PERM_TRANSFER_MANAGER_V2 0x8000000200000000ull     /* SaveDataTransferVersion2 */
#define FS_PERM_TRANSFER_PROHIBITER 0x8000000200002000ull     /* SaveDataTransferVersion2 | CreateSaveData: any application's saves */
#define FS_PERM_OWN_TRANSFER_PROHIBITER 0x8000000800000000ull /* CreateOwnSaveData: the caller's own saves */
#define FS_PERM_TRANSFER_REPAIR 0x8000000200000000ull         /* 85 (and 88, [unverified]: not in the table, 21.0.0+) */
#define FS_PERM_SAVE_MOVER 0x8000001000000000ull              /* MoveCacheStorage */
#define FS_PERM_TRANSFER_FOR_REPAIR 0x8000000000000020ull     /* SaveDataManagement */
#define FS_PERM_MOUNT_CONTENT_STORAGE 0x8000000000000800ull   /* ContentManager */
#define FS_PERM_GET_RIGHTS_ID 0x8000000008000000ull           /* GetRightsId (606, 610) and GetProgramId (618) */

/* 0 OpenFileSystem(u32 FileSystemProxyType; path): removed in 2.0.0 (switchbrew; libnx maps it to OpenFileSystemWithId since). This console runs a
 * current firmware, where the command id does not exist: the answer is the one the dispatcher gives any id the interface lacks. */
static void srv_open_fs_removed(IpcObject *s, IpcCall *c) {
    (void)s;
    c->result = IPC_R_UNKNOWN_COMMAND;
}
/* 31 OpenGameCardFileSystem(u32 GameCardHandle, u32 GameCardPartition: 0 Update, 1 Normal, 2 Secure, 3 Logo). The title runs from installed content and
 * the host has no game card slot, so no card is ever inserted: GameCardCardNotInserted for a process allowed to mount one. */
static void srv_open_game_card_fs(IpcObject *s, IpcCall *c) {
    (void)s;
    FS_REQUIRE(FS_PERM_MOUNT_GAME_CARD);
    if (c->in_size < 8 || IPC_IN(c, uint32_t, 4) > 3) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    c->result = R_FS_GAME_CARD_NOT_INSERTED;
}
/* 80 OpenSaveDataMetaFile(u8 space id, u32 SaveDataMetaType, SaveDataAttribute) -> IFile [field offsets 0 / 4 / 8 unverified]: the meta file the save was
 * created with (SaveDataMetaInfo of 22/35/37), read and written in place. A save that does not exist is TargetNotFound; one created without a meta file
 * of that type is PathNotFound. */
static void srv_open_save_meta_file(IpcObject *s, IpcCall *c) {
    (void)s;
    FS_REQUIRE(FS_PERM_OPEN_SAVE_META_FILE);
    if (c->in_size < 0x48) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    uint32_t type = IPC_IN(c, uint32_t, 4);
    if (type != 1 && type != 2) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    char root[1300], mp[1400];
    save_root(IPC_IN(c, uint8_t, 0), ipc_in(c, 8, 0x40), root, sizeof root);
    SaveMeta m;
    pthread_mutex_lock(&g_meta_mu);
    int found = meta_read(root, &m);
    pthread_mutex_unlock(&g_meta_mu);
    if (!found) {
        c->result = R_FS_TARGET_NOT_FOUND;
        return;
    }
    meta_file_path(root, type, mp, sizeof mp);
    int fd = open(mp, O_RDWR);
    if (fd < 0) {
        c->result = R_FS_PATH_NOT_FOUND;
        return;
    }
    HostFile *f = calloc(1, sizeof *f);
    f->fd = fd;
    f->writable = 1;
    snprintf(f->path, sizeof f->path, "%s", mp);
    ipc_out_object(c, IPC_OBJECT("IFile", file_cmds, f, file_destroy));
}
/* 81 / 82 / 85 / 86 / 87 / 88: the save data transfer managers (console-to-console transfer and repair) and the cache storage mover. Only system tools
 * hold these permissions; an application gets PermissionDenied. A permitted process gets NotImplemented: a transfer needs the peer console and the
 * transfer server, which this single host does not have (the Switch 2 firmware answers 81 the same way). */
static void transfer_unavailable(IpcCall *c, uint64_t mask) {
    FS_REQUIRE(mask);
    c->result = R_FS_NOT_IMPLEMENTED;
}
static void srv_transfer_manager(IpcObject *s, IpcCall *c) {
    (void)s;
    transfer_unavailable(c, FS_PERM_TRANSFER_MANAGER);
}
static void srv_transfer_manager_v2(IpcObject *s, IpcCall *c) {
    (void)s;
    transfer_unavailable(c, FS_PERM_TRANSFER_MANAGER_V2);
}
static void srv_transfer_manager_repair(IpcObject *s, IpcCall *c) {
    (void)s;
    transfer_unavailable(c, FS_PERM_TRANSFER_REPAIR);
}
static void srv_save_mover(IpcObject *s, IpcCall *c) {
    (void)s;
    transfer_unavailable(c, FS_PERM_SAVE_MOVER);
}
static void srv_transfer_manager_for_repair(IpcObject *s, IpcCall *c) {
    (void)s;
    transfer_unavailable(c, FS_PERM_TRANSFER_FOR_REPAIR);
}
/* 83 OpenSaveDataTransferProhibiter(u64 application id) -> ISaveDataTransferProhibiter (no commands): while the object lives, the saves of that application
 * may not be exported. The process keeps the count of live prohibiters per application (fs_transfer_prohibited); a transfer manager would honour it. */
static pthread_mutex_t g_prohibit_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint64_t app;
    unsigned refs;
} g_prohibit[16];
__attribute__((unused)) static unsigned fs_transfer_prohibited(uint64_t app) { /* read by the tests; no exporter exists to consult it */
    unsigned n = 0;
    pthread_mutex_lock(&g_prohibit_mu);
    for (unsigned i = 0; i < 16; i++)
        if (g_prohibit[i].refs && g_prohibit[i].app == app) n = g_prohibit[i].refs;
    pthread_mutex_unlock(&g_prohibit_mu);
    return n;
}
static void prohibiter_destroy(IpcObject *o) {
    uint64_t app = (uint64_t)(uintptr_t)o->state;
    pthread_mutex_lock(&g_prohibit_mu);
    for (unsigned i = 0; i < 16; i++)
        if (g_prohibit[i].refs && g_prohibit[i].app == app) g_prohibit[i].refs--;
    pthread_mutex_unlock(&g_prohibit_mu);
}
/* The interface has no commands. An empty table would make the dispatcher's tolerant stub object, so the one row is a placeholder id no SDK sends, and
 * every real id is "unknown command". */
static void prohibiter_none(IpcObject *s, IpcCall *c) {
    (void)s;
    c->result = IPC_R_UNKNOWN_COMMAND;
}
static const IpcCmd prohibiter_cmds[] = {{0xffffffffu, "(no commands)", prohibiter_none, IPC_IMPL}};
static void srv_transfer_prohibiter(IpcObject *s, IpcCall *c) {
    (void)s;
    if (c->in_size < 8) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    uint64_t app = IPC_IN(c, uint64_t, 0);
    if (!fs_permitted(FS_PERM_TRANSFER_PROHIBITER) && !(app == g_program_id && fs_permitted(FS_PERM_OWN_TRANSFER_PROHIBITER))) {
        c->result = R_FS_PERMISSION_DENIED;
        return;
    }
    int slot = -1;
    pthread_mutex_lock(&g_prohibit_mu);
    for (int i = 0; i < 16 && slot < 0; i++)
        if (g_prohibit[i].refs && g_prohibit[i].app == app) slot = i;
    for (int i = 0; i < 16 && slot < 0; i++)
        if (!g_prohibit[i].refs) slot = i;
    if (slot >= 0) g_prohibit[slot].app = app, g_prohibit[slot].refs++;
    pthread_mutex_unlock(&g_prohibit_mu);
    if (slot < 0) {
        c->result = R_FS_UNEXPECTED;
        return;
    }
    ipc_out_object(c, IPC_OBJECT("ISaveDataTransferProhibiter", prohibiter_cmds, (void *)(uintptr_t)app, prohibiter_destroy));
}
/* 110 OpenContentStorageFileSystem(u32 ContentStorageId: 0 System, 1 User, 2 SdCard, 3 System0 [16.0.0+]) -> IFileSystem of NCA files. Invalid ids are
 * 0x2EE202 (switchbrew). The title runs from its extracted data, so every content storage of this console is an empty, writable directory below the save
 * root (what ncm would write NCA files into). */
static void srv_open_content_storage(IpcObject *s, IpcCall *c) {
    (void)s;
    FS_REQUIRE(FS_PERM_MOUNT_CONTENT_STORAGE);
    static const char *const names[] = {"system", "user", "sdcard", "system0"};
    uint32_t id = c->in_size >= 4 ? IPC_IN(c, uint32_t, 0) : 0xffffffffu;
    if (id > 3) {
        c->result = R_FS_INVALID_ENUM;
        return;
    }
    char root[1300];
    snprintf(root, sizeof root, "%s/%s/contentstorage/%s", g_cfg.save_root, g_cfg.title_id, names[id]);
    ipc_out_object(c, open_host_fs(root, 0, 0, 0));
}
/* 606 GetRightsId(u64 program id, u8 storage id) -> RightsId, 610 GetRightsIdAndKeyGenerationByPath(path) -> u8 key generation + RightsId, 618
 * GetProgramId(path, content attributes) -> u64: facts read from an NCA header. Only system processes hold GetRightsId; an application gets
 * PermissionDenied. A permitted process asks about NCA files the host does not have (the title runs from extracted data, `napple build` does not keep
 * the rights id): content paths are PathNotFound and program ids TargetNotFound, as for content that is not installed. */
static void srv_get_rights_id(IpcObject *s, IpcCall *c) {
    (void)s;
    FS_REQUIRE(FS_PERM_GET_RIGHTS_ID);
    c->result = c->in_size < 9 ? R_FS_INVALID_ARGUMENT : R_FS_TARGET_NOT_FOUND;
}
static void srv_get_by_content_path(IpcObject *s, IpcCall *c) {
    (void)s;
    FS_REQUIRE(FS_PERM_GET_RIGHTS_ID);
    char gp[0x302];
    c->result = guest_path(c, 0, gp, sizeof gp) && *gp ? R_FS_PATH_NOT_FOUND : R_FS_INVALID_ARGUMENT;
}
/* 800 GetAndClearErrorInfo -> FileSystemProxyErrorInfo (0x80, libnx fs.h; raw output before 21.0.0, an output buffer since): the error counters since the
 * last call, then cleared. The host never remounts a RomFS for corruption, has no FAT driver and no game card, so those counters are 0; what is real here
 * is the number of save data in the index (save_data_index_count @0x2C) and the SD card's open file / directory peaks (sd_card_fat_report_info_1 @0x38),
 * reset to the currently open counts by the call. */
static int count_save_cb(const char *root, const SaveMeta *m, void *arg) {
    (void)root;
    (void)m;
    (*(uint32_t *)arg)++;
    return 0;
}
static void srv_get_and_clear_error_info(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t info[0x80] = {0};
    uint32_t saves = 0;
    pthread_mutex_lock(&g_meta_mu);
    meta_walk(host_save_dir(), 6, count_save_cb, &saves);
    pthread_mutex_unlock(&g_meta_mu);
    memcpy(info + 0x2C, &saves, 4);
    pthread_mutex_lock(&g_sd_open_mu);
    uint16_t peaks[2] = {(uint16_t)g_sd_open_peak[0], (uint16_t)g_sd_open_peak[1]};
    g_sd_open_peak[0] = g_sd_open[0];
    g_sd_open_peak[1] = g_sd_open[1];
    pthread_mutex_unlock(&g_sd_open_mu);
    memcpy(info + 0x38, peaks, sizeof peaks);
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (b) ipc_write_buf(c, b, info, b->size < sizeof info ? b->size : sizeof info);
    else ipc_out(c, info, sizeof info);
}
/* 1001 SetSaveDataSize(s64 size, s64 journal size): switchbrew: stored in the fsp-srv session object (defaults 32 MiB / 16 MiB) and not read by anything
 * else, so storing them per connection is the whole behaviour. */
typedef struct {
    int64_t save_size, save_journal_size;
} FspSession;
static void srv_set_save_data_size(IpcObject *s, IpcCall *c) {
    FspSession *fs = s->state;
    if (c->in_size < 16) {
        c->result = R_FS_INVALID_ARGUMENT;
        return;
    }
    if (fs) fs->save_size = IPC_IN(c, int64_t, 0), fs->save_journal_size = IPC_IN(c, int64_t, 8);
}

static const IpcCmd srv_cmds[] = {
    {1, "SetCurrentProcess", srv_noop, IPC_IMPL},
    {18, "OpenSdCardFileSystem", srv_open_sdcard, IPC_IMPL},
    {21, "DeleteSaveDataFileSystem", srv_delete_save, IPC_IMPL},
    {22, "CreateSaveDataFileSystem", srv_create_save, IPC_IMPL},
    {23, "CreateSaveDataFileSystemBySystemSaveDataId", srv_create_save, IPC_IMPL},
    {25, "DeleteSaveDataFileSystemBySaveDataSpaceId", srv_delete_save_by_space, IPC_IMPL},
    {28, "DeleteSaveDataFileSystemBySaveDataAttribute", srv_delete_save_by_attr, IPC_IMPL},
    {32, "ExtendSaveDataFileSystem", srv_extend_save, IPC_IMPL},
    {33, "DeleteCacheStorage", srv_delete_cache, IPC_IMPL},
    {34, "GetCacheStorageSize", srv_cache_size, IPC_IMPL},
    {35, "CreateSaveDataFileSystemWithHashSalt", srv_create_save, IPC_IMPL},
    {51, "OpenSaveDataFileSystem", srv_open_save, IPC_IMPL},
    {52, "OpenSaveDataFileSystemBySystemSaveDataId", srv_open_save, IPC_IMPL},
    {53, "OpenReadOnlySaveDataFileSystem", srv_open_save_read_only, IPC_IMPL},
    {57, "ReadSaveDataFileSystemExtraDataBySaveDataSpaceId", srv_read_extra_by_space, IPC_IMPL},
    {58, "ReadSaveDataFileSystemExtraData", srv_read_extra, IPC_IMPL},
    {59, "WriteSaveDataFileSystemExtraData", srv_write_extra, IPC_IMPL},
    {60, "OpenSaveDataInfoReader", srv_open_reader, IPC_IMPL},
    {61, "OpenSaveDataInfoReaderBySaveDataSpaceId", srv_open_reader_space, IPC_IMPL},
    {62, "OpenSaveDataInfoReaderOnlyCacheStorage", srv_open_reader_cache, IPC_IMPL},
    {66, "WriteSaveDataFileSystemExtraDataWithMask", srv_write_extra_mask, IPC_IMPL},
    {69, "ReadSaveDataFileSystemExtraDataBySaveDataAttribute", srv_read_extra_by_attr, IPC_IMPL},
    {70, "WriteSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute", srv_write_extra_mask_by_attr, IPC_IMPL},
    {71, "ReadSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute", srv_read_extra_mask_by_attr, IPC_IMPL},
    {30, "OpenGameCardStorage", sysd_err_generic, IPC_STUB},
    {200, "OpenDataStorageByCurrentProcess", srv_open_data_storage, IPC_IMPL},
    {203, "OpenPatchDataStorageByCurrentProcess", srv_open_patch_storage, IPC_IMPL},
    {202, "OpenDataStorageByDataId", srv_open_data_storage_by_id, IPC_IMPL},
    /* The console's own behaviour for an offline application: auto save creation is a system setting that nothing here enables, the global
     * access log mode is 0 (off) and nothing is written to the SD card, so the no-op / 0 answers are the real ones, not placeholders. */
    {1003, "DisableAutoSaveDataCreation", srv_noop, IPC_IMPL},
    {1005, "GetGlobalAccessLogMode", srv_get_log_mode, IPC_IMPL},
    {1006, "OutputAccessLogToSdCard", srv_noop, IPC_IMPL},
    {0, "OpenFileSystem", srv_open_fs_removed, IPC_IMPL},
    {2, "OpenDataFileSystemByCurrentProcess", srv_open_data_fs_current, IPC_IMPL},
    {7, "OpenFileSystemWithPatch", srv_open_fs_with_patch, IPC_IMPL},
    {9, "OpenDataFileSystemByProgramId", srv_open_data_fs_by_program, IPC_IMPL},
    {10, "OpenFileSystemWithId", srv_open_fs_with_id, IPC_IMPL},
    {27, "IsExFatSupported", srv_is_exfat_supported, IPC_IMPL},
    {31, "OpenGameCardFileSystem", srv_open_game_card_fs, IPC_IMPL},
    {37, "CreateSaveDataFileSystemWithCreationInfo2", srv_create_save2, IPC_IMPL},
    {64, "OpenSaveDataInternalStorageFileSystem", srv_open_internal_storage, IPC_IMPL},
    {67, "FindSaveDataWithFilter", srv_find_with_filter, IPC_IMPL},
    {68, "OpenSaveDataInfoReaderWithFilter", srv_open_reader_filter, IPC_IMPL},
    {80, "OpenSaveDataMetaFile", srv_open_save_meta_file, IPC_IMPL},
    {81, "OpenSaveDataTransferManager", srv_transfer_manager, IPC_IMPL},
    {82, "OpenSaveDataTransferManagerVersion2", srv_transfer_manager_v2, IPC_IMPL},
    {83, "OpenSaveDataTransferProhibiter", srv_transfer_prohibiter, IPC_IMPL},
    /* 84: the 0x10-byte input is not documented, and the permitted path lists the FsAccessControl save-data owner ids, which npdm.c does not parse */
    {84, "ListAccessibleSaveDataOwnerId", sysd_err_generic, IPC_STUB},
    {85, "OpenSaveDataTransferManagerForSaveDataRepair", srv_transfer_manager_repair, IPC_IMPL},
    {86, "OpenSaveDataMover", srv_save_mover, IPC_IMPL},
    {87, "OpenSaveDataTransferManagerForRepair", srv_transfer_manager_for_repair, IPC_IMPL},
    {88, "OpenSaveDataTransferManagerForSaveDataRepairWithPolicy", srv_transfer_manager_repair, IPC_IMPL},
    {100, "OpenImageDirectoryFileSystem", srv_open_image_directory, IPC_IMPL},
    {110, "OpenContentStorageFileSystem", srv_open_content_storage, IPC_IMPL},
    {130, "OpenCustomStorageFileSystem", srv_open_custom_storage, IPC_IMPL},
    {201, "OpenDataStorageByProgramId", srv_open_data_storage_by_program, IPC_IMPL},
    {205, "OpenDataStorageWithProgramIndex", srv_open_data_storage_with_index, IPC_IMPL},
    {510, "OpenSystemDataUpdateEventNotifier", srv_open_sysdata_notifier, IPC_IMPL},
    {511, "NotifySystemDataUpdateEvent", srv_notify_sysdata_update, IPC_IMPL},
    {606, "GetRightsId", srv_get_rights_id, IPC_IMPL},
    {610, "GetRightsIdAndKeyGenerationByPath", srv_get_by_content_path, IPC_IMPL},
    {612, "GetFreeSpaceSizeForSaveData", srv_free_space_for_save, IPC_IMPL},
    {616, "GetSaveDataCommitId", srv_get_commit_id, IPC_IMPL},
    {618, "GetProgramId", srv_get_by_content_path, IPC_IMPL},
    {620, "SetSdCardEncryptionSeed", srv_set_sd_encryption_seed, IPC_IMPL},
    {630, "SetSdCardAccessibility", srv_set_sd_accessibility, IPC_IMPL},
    {631, "IsSdCardAccessible", srv_is_sd_accessible, IPC_IMPL},
    {800, "GetAndClearErrorInfo", srv_get_and_clear_error_info, IPC_IMPL},
    {1001, "SetSaveDataSize", srv_set_save_data_size, IPC_IMPL},
    {1004, "SetGlobalAccessLogMode", srv_set_log_mode, IPC_IMPL},
    {1011, "GetProgramIndexForAccessLog", srv_get_program_index_for_access_log, IPC_IMPL},
    {1012, "GetFsStackUsage", sysd_z8, IPC_STUB}, /* the stack peak of an fs-internal thread type (enum not documented); the host has no such threads */
    {1016, "FlushAccessLogOnSdCard", srv_noop, IPC_IMPL}, /* the access log is never written (mode 0 unless the guest sets it, and nothing is buffered) */
};
static void fsp_destroy(IpcObject *o) { free(o->state); }
static IpcObject *fsp_open(void) {
    FspSession *s = calloc(1, sizeof *s);
    if (s) s->save_size = 0x2000000, s->save_journal_size = 0x1000000; /* the defaults switchbrew gives for SetSaveDataSize */
    return IPC_OBJECT("fsp-srv", srv_cmds, s, fsp_destroy);
}
void service_register_fs(void) { ipc_register_service("fsp-srv", fsp_open); }

/* Called when the process stops: tells which transactional save roots still hold changes the game never committed (those are rolled back
 * at the next start, exactly like on the console). */
void fs_report_uncommitted(void) {
    for (int i = 0; i < 16 && g_journals[i]; i++)
        if (g_journals[i]->dirty)
            LOG("fs", "save %s: changes since the last commit are discarded (%u commits this run)", g_journals[i]->live, g_journals[i]->commits);
        else if (g_journals[i]->commits) LOG("fs", "save %s: %u commits this run, nothing pending", g_journals[i]->live, g_journals[i]->commits);
    cloud_on_exit();
}
