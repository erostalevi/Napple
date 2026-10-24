/* fsp-srv: the game's file systems on macOS.
 *   RomFS     OpenDataStorageByCurrentProcess serves the raw RomFS image (<data-dir>/romfs.bin, the decrypted IVFC data level)
 *             as an IStorage: the SDK parses the RomFS tables itself, the host only answers reads.
 *   save data OpenSaveDataFileSystem maps to directories under <save-root>/<titleid>/save/<space>/<type>[/<user>].
 *   sdcard    <save-root>/<titleid>/sdcard.
 * Paths are validated (no "..", no NUL, bounded length) before they touch the host file system. */
#include "../ipc.h"
#include "host_info.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef __APPLE__
#include <sys/clonefile.h>
#define st_created(sb) ((sb).st_birthtime)
#else   /* the host unit tests (runtime/tests/host) build this file on Linux: no clones, the recursive copy is used */
static int clonefile(const char *src, const char *dst, int flags) { (void)src; (void)dst; (void)flags; errno = ENOTSUP; return -1; }
#define st_created(sb) ((sb).st_ctime)
#endif
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define FS_RESULT(desc) (2u | ((desc) << 9))
#define R_FS_PATH_NOT_FOUND FS_RESULT(1)
#define R_FS_PATH_EXISTS FS_RESULT(2)
#define R_FS_TARGET_NOT_FOUND FS_RESULT(1002)
#define R_FS_TARGET_LOCKED FS_RESULT(7)
#define R_FS_DIR_NOT_EMPTY FS_RESULT(8)
#define R_FS_NOT_ENOUGH_SPACE FS_RESULT(3)
#define R_FS_INVALID_ARGUMENT FS_RESULT(6002)
#define R_FS_NOT_FOUND_DATA FS_RESULT(1001)
#define R_FS_UNEXPECTED FS_RESULT(5000)

static uint32_t errno_result(void) {
    switch (errno) {
    case ENOENT: case ENOTDIR: return R_FS_PATH_NOT_FOUND;
    case EEXIST: return R_FS_PATH_EXISTS;
    case ENOTEMPTY: return R_FS_DIR_NOT_EMPTY;
    case ENOSPC: return R_FS_NOT_ENOUGH_SPACE;
    case EBUSY: case ETXTBSY: return R_FS_TARGET_LOCKED;
    default: return R_FS_UNEXPECTED;
    }
}

/* ---- host path mapping ---------------------------------------------------------------------------------------- */
typedef struct SaveJournal { char live[1100], committed[1120], tmp[1120], old[1120], marker[1120]; int ready; volatile int dirty; unsigned commits; pthread_mutex_t mu; } SaveJournal;
static void journal_mark_dirty(SaveJournal *j) { if (j) j->dirty = 1; }
typedef struct { char root[1024]; int read_only; SaveJournal *journal; int64_t quota; } HostFs;   /* quota: save data size, 0 unknown */

/* Maps a guest path ("/dir/file", "dir/file") below the file system root; 0 when the path is not acceptable. */
static int host_path(const HostFs *fs, const char *guest, char *out, size_t cap) {
    if (!guest) return 0;
    size_t n = strnlen(guest, 0x302);
    if (n == 0 || n > 0x300) return 0;
    char norm[0x302]; size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = guest[i] == '\\' ? '/' : guest[i];
        if (ch == ':' && i + 1 < n && guest[i + 1] == '/') { k = 0; continue; }   /* "save:/x" style mount prefix */
        if (ch == '/' && k && norm[k - 1] == '/') continue;
        norm[k++] = ch;
    }
    norm[k] = 0;
    for (char *p = norm; *p;) {                           /* reject any ".." component */
        char *e = strchr(p, '/'); size_t len = e ? (size_t)(e - p) : strlen(p);
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
    memcpy(buf, p, len); buf[len] = 0;
    return buf;
}

static void mkdirs(const char *path) {
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

/* ---- IFile --------------------------------------------------------------------------------------------------- */
typedef struct { int fd; int writable; char path[1100]; SaveJournal *journal; } HostFile;
static void file_destroy(IpcObject *o) { HostFile *f = o->state; if (f) { if (f->fd >= 0) close(f->fd); free(f); } }
static void file_read(IpcObject *self, IpcCall *c) {
    HostFile *f = self->state; int64_t off = IPC_IN(c, int64_t, 8), size = IPC_IN(c, int64_t, 16);
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!b || off < 0 || size < 0) { c->result = R_FS_INVALID_ARGUMENT; return; }
    if ((uint64_t)size > b->size) size = (int64_t)b->size;
    void *dst = guest_ptr(b->addr, (uint64_t)size);
    if (!dst) { c->result = R_FS_INVALID_ARGUMENT; return; }
    guest_write_prepare(b->addr, (size_t)size);
    ssize_t n = pread(f->fd, dst, (size_t)size, off);
    if (n < 0) { c->result = errno_result(); return; }
    uint64_t got = (uint64_t)n;
    IPC_OUT(c, got);
}
static void file_write(IpcObject *self, IpcCall *c) {
    HostFile *f = self->state; int64_t off = IPC_IN(c, int64_t, 8), size = IPC_IN(c, int64_t, 16);
    const IpcBuf *b = ipc_in_buf(c, 0);
    if (!f->writable) { c->result = R_FS_INVALID_ARGUMENT; return; }
    if (!b || off < 0 || size < 0 || (uint64_t)size > b->size) { c->result = R_FS_INVALID_ARGUMENT; return; }
    const void *src = guest_ptr(b->addr, (uint64_t)size);
    if (!src) { c->result = R_FS_INVALID_ARGUMENT; return; }
    if (pwrite(f->fd, src, (size_t)size, off) != size) c->result = errno_result();
    else journal_mark_dirty(f->journal);
}
static void file_flush(IpcObject *self, IpcCall *c) { HostFile *f = self->state; if (f->writable) fsync(f->fd); (void)c; }
static void file_set_size(IpcObject *self, IpcCall *c) { HostFile *f = self->state; journal_mark_dirty(f->journal); if (ftruncate(f->fd, IPC_IN(c, int64_t, 0))) c->result = errno_result(); }
static void file_get_size(IpcObject *self, IpcCall *c) {
    HostFile *f = self->state; struct stat sb;
    if (fstat(f->fd, &sb)) { c->result = errno_result(); return; }
    int64_t v = sb.st_size; IPC_OUT(c, v);
}
static const IpcCmd file_cmds[] = {
    {0, "Read", file_read, IPC_IMPL}, {1, "Write", file_write, IPC_IMPL}, {2, "Flush", file_flush, IPC_IMPL},
    {3, "SetSize", file_set_size, IPC_IMPL}, {4, "GetSize", file_get_size, IPC_IMPL},
};

/* ---- IDirectory ---------------------------------------------------------------------------------------------- */
typedef struct { DIR *d; int mode; char path[1100]; } HostDir;
static void dir_destroy(IpcObject *o) { HostDir *d = o->state; if (d) { closedir(d->d); free(d); } }
static void dir_read(IpcObject *self, IpcCall *c) {
    HostDir *d = self->state; const IpcBuf *b = ipc_out_buf(c, 0);
    if (!b) { c->result = R_FS_INVALID_ARGUMENT; return; }
    uint8_t *out = guest_ptr(b->addr, b->size);
    if (!out) { c->result = R_FS_INVALID_ARGUMENT; return; }
    int64_t max = (int64_t)(b->size / 0x310), n = 0; struct dirent *e;
    guest_write_prepare(b->addr, (size_t)b->size);
    while (n < max && (e = readdir(d->d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        int isdir = e->d_type == DT_DIR;
        if ((isdir && !(d->mode & 1)) || (!isdir && !(d->mode & 2))) continue;
        uint8_t *ent = out + n * 0x310;
        memset(ent, 0, 0x310);
        strncpy((char *)ent, e->d_name, 0x300);
        ent[0x304] = isdir ? 0 : 1;
        if (!isdir) { char p[1400]; struct stat sb; snprintf(p, sizeof p, "%s/%s", d->path, e->d_name); if (!stat(p, &sb)) { int64_t sz = sb.st_size; memcpy(ent + 0x308, &sz, 8); } }
        n++;
    }
    IPC_OUT(c, n);
}
static void dir_entry_count(IpcObject *self, IpcCall *c) {
    HostDir *d = self->state; long pos = telldir(d->d); rewinddir(d->d);
    int64_t n = 0; struct dirent *e;
    while ((e = readdir(d->d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        int isdir = e->d_type == DT_DIR;
        if ((isdir && (d->mode & 1)) || (!isdir && (d->mode & 2))) n++;
    }
    seekdir(d->d, pos); IPC_OUT(c, n);
}
static const IpcCmd dir_cmds[] = {{0, "Read", dir_read, IPC_IMPL}, {1, "GetEntryCount", dir_entry_count, IPC_IMPL}};

/* ---- IFileSystem --------------------------------------------------------------------------------------------- */
static int rm_rf(const char *p, int keep_root) {
    DIR *d = opendir(p);
    if (!d) return unlink(p);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[1400]; snprintf(child, sizeof child, "%s/%s", p, e->d_name);
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
static SaveJournal *g_journals[16]; static pthread_mutex_t g_journals_mu = PTHREAD_MUTEX_INITIALIZER;
static int path_exists(const char *p) { struct stat sb; return lstat(p, &sb) == 0; }
static int copy_tree(const char *src, const char *dst) {      /* fallback when clonefile is unavailable (not APFS) */
    struct stat sb; if (lstat(src, &sb)) return -1;
    if (S_ISDIR(sb.st_mode)) {
        if (mkdir(dst, 0755) && errno != EEXIST) return -1;
        DIR *d = opendir(src); if (!d) return -1; struct dirent *e; int rc = 0;
        while (!rc && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char a[1400], b[1400]; snprintf(a, sizeof a, "%s/%s", src, e->d_name); snprintf(b, sizeof b, "%s/%s", dst, e->d_name);
            rc = copy_tree(a, b);
        }
        closedir(d); return rc;
    }
    int in = open(src, O_RDONLY), out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644); int rc = in < 0 || out < 0 ? -1 : 0; char buf[65536]; ssize_t n;
    while (!rc && (n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)n) != n) rc = -1;
    if (n < 0) rc = -1;
    if (in >= 0) close(in);
    if (out >= 0 && (fsync(out) || close(out))) rc = -1;
    return rc;
}
static int clone_tree(const char *src, const char *dst) { return clonefile(src, dst, 0) == 0 ? 0 : copy_tree(src, dst); }
static void journal_recover(SaveJournal *j) {
    if (!path_exists(j->committed) && path_exists(j->old)) rename(j->old, j->committed);       /* crash between the two renames of a commit */
    rm_rf(j->tmp, 0); rm_rf(j->old, 0);
    if (path_exists(j->committed)) {
        /* live := clone(committed), built beside the live directory first: if the clone fails (permissions, disk full) the existing directory stays
         * untouched instead of being lost with nothing to replace it */
        char staging[1130]; snprintf(staging, sizeof staging, "%s.restoring", j->live);
        rm_rf(staging, 0);
        if (clone_tree(j->committed, staging)) { LOG("fs", "save %s: cannot restore the committed state: %s (keeping the directory as it is)", j->live, strerror(errno)); rm_rf(staging, 0); }
        else { rm_rf(j->live, 0); if (rename(staging, j->live)) LOG("fs", "save %s: cannot move the restored state into place: %s", j->live, strerror(errno)); else LOGV("fs", "save %s: restored from the last committed snapshot", j->live); }
    } else if (!path_exists(j->marker) && path_exists(j->live)) {                                 /* pre-journal save: adopt it */
        if (clone_tree(j->live, j->committed)) LOG("fs", "save %s: cannot take the first snapshot: %s", j->live, strerror(errno));
    } else { rm_rf(j->live, 0); }                                                                 /* marker but nothing ever committed */
    mkdirs(j->live);
    int fd = open(j->marker, O_CREAT | O_WRONLY, 0644); if (fd >= 0) close(fd);
}
static SaveJournal *journal_get(const char *live) {
    pthread_mutex_lock(&g_journals_mu);
    SaveJournal *j = NULL;
    for (int i = 0; i < 16 && g_journals[i]; i++) if (!strcmp(g_journals[i]->live, live)) j = g_journals[i];
    if (!j) {
        for (int i = 0; i < 16; i++) if (!g_journals[i]) {
            j = calloc(1, sizeof *j); pthread_mutex_init(&j->mu, NULL); g_journals[i] = j;
            snprintf(j->live, sizeof j->live, "%s", live);
            snprintf(j->committed, sizeof j->committed, "%s.committed", live); snprintf(j->tmp, sizeof j->tmp, "%s.committing", live);
            snprintf(j->old, sizeof j->old, "%s.previous", live); snprintf(j->marker, sizeof j->marker, "%s.napple-journal", live);
            break;
        }
    }
    pthread_mutex_unlock(&g_journals_mu);
    if (!j) LOG("fs", "save %s: more than 16 transactional save roots; this one is not transactional", live);
    if (j) { pthread_mutex_lock(&j->mu); if (!j->ready) { char parent[1100]; snprintf(parent, sizeof parent, "%s", live); char *sl = strrchr(parent, '/'); if (sl) { *sl = 0; mkdirs(parent); } journal_recover(j); j->ready = 1; } pthread_mutex_unlock(&j->mu); }
    return j;
}
static uint32_t journal_commit(SaveJournal *j) {
    pthread_mutex_lock(&j->mu);
    uint32_t r = 0;
    int was_dirty = j->dirty;
    j->dirty = 0;                     /* before the snapshot: a write racing with the commit marks the root dirty again */
    rm_rf(j->tmp, 0);
    if (clone_tree(j->live, j->tmp)) { r = errno_result(); LOG("fs", "commit %s: snapshot failed: %s", j->live, strerror(errno)); rm_rf(j->tmp, 0); }
    else {
        if (path_exists(j->committed) && rename(j->committed, j->old)) { r = errno_result(); rm_rf(j->tmp, 0); }
        else if (rename(j->tmp, j->committed)) { r = errno_result(); if (path_exists(j->old)) rename(j->old, j->committed); rm_rf(j->tmp, 0); }
        else rm_rf(j->old, 0);
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
    uint8_t attr[0x40]; uint64_t owner_id; int64_t timestamp; uint32_t flags, pad; int64_t data_size, journal_size, commit_id; uint8_t unused[0x190];
} SaveExtraData;
_Static_assert(sizeof(SaveExtraData) == 0x200, "SaveDataExtraData layout");
typedef struct { char magic[4]; uint32_t version; uint64_t id; uint8_t space, implicit, pad[6]; SaveExtraData extra; } SaveMeta;   /* implicit: made on first use, not by Create */
#define META_SUFFIX ".napple-meta"
enum { SAVE_SYSTEM = 0, SAVE_ACCOUNT = 1, SAVE_BCAT = 2, SAVE_DEVICE = 3, SAVE_TEMPORARY = 4, SAVE_CACHE = 5 };
static pthread_mutex_t g_meta_mu = PTHREAD_MUTEX_INITIALIZER;

/* <save>/space<N>/<type>[/<system save id>][/<user>][/idx<index>] for SaveDataAttribute {u64 program, u128 user, u64 system id, u8 type,
 * u8 rank, u16 index}. */
static void save_root(uint8_t space, const uint8_t *attr, char *root, size_t cap) {
    static const char *const types[] = {"system", "account", "bcat", "device", "temporary", "cache"};
    uint8_t type = attr[0x20]; uint16_t index; memcpy(&index, attr + 0x22, 2);
    uint64_t sid; memcpy(&sid, attr + 0x18, 8);
    const uint8_t *uid = attr + 8;
    int has_uid = 0; for (int i = 0; i < 16; i++) if (uid[i]) has_uid = 1;
    int n = snprintf(root, cap, "%s/space%u/%s", host_save_dir(), space, type < 6 ? types[type] : "other");
    if (type == SAVE_SYSTEM && sid) n += snprintf(root + n, cap - (size_t)n, "/%016llx", (unsigned long long)sid);
    if (has_uid) n += snprintf(root + n, cap - (size_t)n, "/%02x%02x%02x%02x", uid[0], uid[1], uid[2], uid[3]);
    if (index) snprintf(root + n, cap - (size_t)n, "/idx%u", index);
}
static uint64_t save_id_for(const char *root) {
    const char *rel = root + strlen(host_save_dir());
    uint64_t h = 1469598103934665603ull; for (; *rel; rel++) h = (h ^ (uint8_t)*rel) * 1099511628211ull;
    return (h & 0x0000ffffffffffffull) | 0x0100000000000000ull;   /* never 0 (0 means "no id" to fs) */
}
static int meta_read(const char *root, SaveMeta *m) {
    char p[1300]; snprintf(p, sizeof p, "%s" META_SUFFIX, root);
    int fd = open(p, O_RDONLY); if (fd < 0) return 0;
    ssize_t n = read(fd, m, sizeof *m); close(fd);
    return n == (ssize_t)sizeof *m && !memcmp(m->magic, "NSMD", 4);
}
static int meta_write(const char *root, const SaveMeta *m) {
    char p[1300], tmp[1320]; snprintf(p, sizeof p, "%s" META_SUFFIX, root); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY, 0644); if (fd < 0) return -1;
    int ok = write(fd, m, sizeof *m) == (ssize_t)sizeof *m && !fsync(fd);
    close(fd);
    if (!ok || rename(tmp, p)) { unlink(tmp); return -1; }      /* atomic replace: a crash keeps the old record */
    return 0;
}
static void meta_init(SaveMeta *m, const char *root, uint8_t space, const uint8_t *attr) {
    memset(m, 0, sizeof *m); memcpy(m->magic, "NSMD", 4); m->version = 1; m->id = save_id_for(root); m->space = space;
    memcpy(m->extra.attr, attr, 0x40); m->extra.timestamp = (int64_t)time(NULL);
}
/* Meta of an existing save root (created with defaults when the directory exists without one). 0 when there is no such save. */
static int meta_get(const char *root, uint8_t space, const uint8_t *attr, SaveMeta *m) {
    if (meta_read(root, m)) return 1;
    struct stat sb;
    if (stat(root, &sb) || !S_ISDIR(sb.st_mode)) return 0;
    meta_init(m, root, space, attr); m->implicit = 1;
    return meta_write(root, m) == 0;
}
/* Walks the save tree for meta files; calls fn(root, meta, arg) for each until it returns nonzero. */
static int meta_walk(const char *dir, int depth, int (*fn)(const char *, const SaveMeta *, void *), void *arg) {
    DIR *d = opendir(dir); if (!d) return 0;
    struct dirent *e; int stop = 0;
    while (!stop && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1400]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        size_t n = strlen(p), k = strlen(META_SUFFIX);
        if (n > k && !strcmp(p + n - k, META_SUFFIX)) {
            p[n - k] = 0; SaveMeta m;
            if (meta_read(p, &m)) stop = fn(p, &m, arg);
            continue;
        }
        struct stat sb;
        if (depth > 0 && !lstat(p, &sb) && S_ISDIR(sb.st_mode) && !strstr(e->d_name, ".committed") && !strstr(e->d_name, ".previous") && !strstr(e->d_name, ".committing"))
            stop = meta_walk(p, depth - 1, fn, arg);
    }
    closedir(d);
    return stop;
}
typedef struct { uint64_t id; int space; char root[1300]; SaveMeta meta; int found; } FindById;
static int find_by_id_cb(const char *root, const SaveMeta *m, void *arg) {
    FindById *f = arg;
    if (m->id != f->id || (f->space >= 0 && m->space != f->space)) return 0;
    snprintf(f->root, sizeof f->root, "%s", root); f->meta = *m; f->found = 1; return 1;
}
static int find_by_id(int space, uint64_t id, char *root, size_t cap, SaveMeta *m) {
    FindById f = {.id = id, .space = space};
    meta_walk(host_save_dir(), 6, find_by_id_cb, &f);
    if (f.found) { snprintf(root, cap, "%s", f.root); *m = f.meta; }
    return f.found;
}
/* Directory usage in bytes, rounded to 16 KiB blocks per file (how save data counts its space). */
static int64_t tree_usage(const char *p) {
    struct stat sb; if (lstat(p, &sb)) return 0;
    if (!S_ISDIR(sb.st_mode)) return (sb.st_size + 0x3fff) & ~0x3fffll;
    int64_t total = 0; DIR *d = opendir(p); if (!d) return 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[1400]; snprintf(c, sizeof c, "%s/%s", p, e->d_name); total += tree_usage(c);
    }
    closedir(d);
    return total;
}
static int save_type_journaled(uint8_t type) { return type == SAVE_SYSTEM || type == SAVE_ACCOUNT || type == SAVE_DEVICE || type == SAVE_CACHE; }
/* Removes a save root with its snapshot, journal marker and meta; the in-process journal starts over if the root is created again. */
static void save_delete(const char *root) {
    char p[1400];
    rm_rf(root, 0);
    snprintf(p, sizeof p, "%s.committed", root); rm_rf(p, 0);
    snprintf(p, sizeof p, "%s.napple-journal", root); unlink(p);
    snprintf(p, sizeof p, "%s" META_SUFFIX, root); unlink(p);
    pthread_mutex_lock(&g_journals_mu);
    for (int i = 0; i < 16 && g_journals[i]; i++) if (!strcmp(g_journals[i]->live, root)) { g_journals[i]->ready = 0; g_journals[i]->dirty = 0; }
    pthread_mutex_unlock(&g_journals_mu);
}

#define FS_PATH(var, n) \
    char gp##n[0x302], var[1400]; \
    if (!guest_path(c, n, gp##n, sizeof gp##n) || !host_path(fs, gp##n, var, sizeof var)) { c->result = R_FS_INVALID_ARGUMENT; return; }
static int fs_trace(void) { static int v = -1; if (v < 0) v = getenv("NAPPLE_FS_TRACE") != NULL; return v; }
#define FS_WRITE_CHECK() if (fs->read_only) { c->result = R_FS_INVALID_ARGUMENT; return; }

static void fs_create_file(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state; FS_WRITE_CHECK(); journal_mark_dirty(fs->journal); FS_PATH(p, 0);
    int fd = open(p, O_CREAT | O_EXCL | O_RDWR, 0644);
    if (fs_trace()) LOG("fs", "CreateFile %s -> %s", p, fd < 0 ? strerror(errno) : "ok");
    if (fd < 0) { c->result = errno_result(); return; }
    if (ftruncate(fd, IPC_IN(c, int64_t, 8))) c->result = errno_result();
    close(fd);
}
static void fs_delete_file(IpcObject *s, IpcCall *c) { HostFs *fs = s->state; FS_WRITE_CHECK(); journal_mark_dirty(fs->journal); FS_PATH(p, 0); if (fs_trace()) LOG("fs", "DeleteFile %s", p); if (unlink(p)) c->result = errno_result(); }
static void fs_create_directory(IpcObject *s, IpcCall *c) { HostFs *fs = s->state; FS_WRITE_CHECK(); journal_mark_dirty(fs->journal); FS_PATH(p, 0); if (mkdir(p, 0755)) c->result = errno_result(); }
static void fs_delete_directory(IpcObject *s, IpcCall *c) { HostFs *fs = s->state; FS_WRITE_CHECK(); journal_mark_dirty(fs->journal); FS_PATH(p, 0); if (rmdir(p)) c->result = errno_result(); }
static void fs_delete_directory_recursively(IpcObject *s, IpcCall *c) { HostFs *fs = s->state; FS_WRITE_CHECK(); journal_mark_dirty(fs->journal); FS_PATH(p, 0); if (rm_rf(p, 0)) c->result = errno_result(); }
static void fs_rename(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state; FS_WRITE_CHECK(); journal_mark_dirty(fs->journal); FS_PATH(p, 0); FS_PATH(q, 1);
    if (fs_trace()) LOG("fs", "Rename %s -> %s", p, q);
    if (rename(p, q)) c->result = errno_result();
}
static void fs_get_entry_type(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state; FS_PATH(p, 0); struct stat sb;
    if (stat(p, &sb)) { c->result = R_FS_PATH_NOT_FOUND; return; }
    uint32_t t = S_ISDIR(sb.st_mode) ? 0 : 1; IPC_OUT(c, t);
}
static void fs_open_file(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state; FS_PATH(p, 0);
    uint32_t mode = IPC_IN(c, uint32_t, 0);
    int writable = (mode & 6) != 0;
    if (writable && fs->read_only) { c->result = R_FS_INVALID_ARGUMENT; return; }
    int fd = open(p, writable ? O_RDWR : O_RDONLY);
    if (fd < 0) { c->result = errno_result(); return; }
    HostFile *f = calloc(1, sizeof *f);
    f->fd = fd; f->writable = writable; f->journal = fs->journal; snprintf(f->path, sizeof f->path, "%s", p);
    ipc_out_object(c, IPC_OBJECT("IFile", file_cmds, f, file_destroy));
}
static void fs_open_directory(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state; FS_PATH(p, 0);
    DIR *d = opendir(p);
    if (!d) { c->result = errno_result(); return; }
    HostDir *hd = calloc(1, sizeof *hd);
    hd->d = d; hd->mode = (int)IPC_IN(c, uint32_t, 0); snprintf(hd->path, sizeof hd->path, "%s", p);
    ipc_out_object(c, IPC_OBJECT("IDirectory", dir_cmds, hd, dir_destroy));
}
static void fs_commit(IpcObject *s, IpcCall *c) { HostFs *fs = s->state; if (fs_trace()) LOG("fs", "Commit %s%s", fs->root, fs->journal ? (fs->journal->dirty ? " (changes pending)" : " (clean)") : " (not transactional)"); if (fs->journal) { uint32_t r = journal_commit(fs->journal); if (r) c->result = r; } else sync(); }
/* Sizes: a save created with a size answers from it (free = size - blocks in use); everything else reports 1 GiB. */
#define FS_DEFAULT_SPACE 0x40000000ll
static void fs_free_space(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state; int64_t v = FS_DEFAULT_SPACE;
    if (fs->quota > 0) { v = fs->quota - tree_usage(fs->root); if (v < 0) v = 0; }
    IPC_OUT(c, v);
}
static void fs_total_space(IpcObject *s, IpcCall *c) { HostFs *fs = s->state; int64_t v = fs->quota > 0 ? fs->quota : FS_DEFAULT_SPACE; IPC_OUT(c, v); }
static void fs_clean_directory(IpcObject *s, IpcCall *c) { HostFs *fs = s->state; FS_WRITE_CHECK(); journal_mark_dirty(fs->journal); FS_PATH(p, 0); rm_rf(p, 1); }
static void fs_get_timestamp(IpcObject *s, IpcCall *c) {
    HostFs *fs = s->state; FS_PATH(p, 0); struct stat sb;
    if (stat(p, &sb)) { c->result = R_FS_PATH_NOT_FOUND; return; }
    int64_t t[3] = {st_created(sb), sb.st_mtime, sb.st_atime}; uint8_t local[8] = {0};
    ipc_out(c, t, sizeof t); ipc_out(c, local, 8);
}
static const IpcCmd fs_cmds[] = {
    {0, "CreateFile", fs_create_file, IPC_IMPL}, {1, "DeleteFile", fs_delete_file, IPC_IMPL}, {2, "CreateDirectory", fs_create_directory, IPC_IMPL},
    {3, "DeleteDirectory", fs_delete_directory, IPC_IMPL}, {4, "DeleteDirectoryRecursively", fs_delete_directory_recursively, IPC_IMPL},
    {5, "RenameFile", fs_rename, IPC_IMPL}, {6, "RenameDirectory", fs_rename, IPC_IMPL}, {7, "GetEntryType", fs_get_entry_type, IPC_IMPL},
    {8, "OpenFile", fs_open_file, IPC_IMPL}, {9, "OpenDirectory", fs_open_directory, IPC_IMPL}, {10, "Commit", fs_commit, IPC_IMPL},
    {11, "GetFreeSpaceSize", fs_free_space, IPC_IMPL}, {12, "GetTotalSpaceSize", fs_total_space, IPC_IMPL}, {13, "CleanDirectoryRecursively", fs_clean_directory, IPC_IMPL},
    {14, "GetFileTimeStampRaw", fs_get_timestamp, IPC_IMPL},
};
static IpcObject *open_host_fs(const char *root, int read_only, int journaled, int64_t quota) {
    HostFs *fs = calloc(1, sizeof *fs);
    snprintf(fs->root, sizeof fs->root, "%s", root); fs->read_only = read_only; fs->quota = quota;
    if (journaled) fs->journal = journal_get(root);
    mkdirs(root);
    LOG("fs", "mounted host directory %s%s", root, read_only ? " (read only)" : "");
    return IPC_OBJECT("IFileSystem", fs_cmds, fs, NULL);
}

/* ---- IStorage over romfs.bin ------------------------------------------------------------------------------------ */
typedef struct { int fd; uint64_t size; } StorageFile;
static void storage_destroy(IpcObject *o) { StorageFile *s = o->state; if (s) { close(s->fd); free(s); } }
static void storage_read(IpcObject *self, IpcCall *c) {
    StorageFile *st = self->state; int64_t off = IPC_IN(c, int64_t, 0), size = IPC_IN(c, int64_t, 8);
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!b || off < 0 || size < 0 || (uint64_t)off > st->size || (uint64_t)size > b->size) { c->result = FS_RESULT(4002); return; }
    if ((uint64_t)off + (uint64_t)size > st->size) { c->result = FS_RESULT(4002); return; }   /* out of range */
    void *dst = guest_ptr(b->addr, (uint64_t)size);
    if (!dst && size) { c->result = R_FS_INVALID_ARGUMENT; return; }
    if (size >= 0x10000 && getenv("NAPPLE_FS_TRACE")) LOG("fs", "RomFS read offset %#llx size %#llx", (unsigned long long)off, (unsigned long long)size);
    guest_write_prepare(b->addr, (size_t)size);
    for (int64_t done = 0; done < size;) {
        ssize_t n = pread(st->fd, (char *)dst + done, (size_t)(size - done), off + done);
        if (n <= 0) { c->result = R_FS_UNEXPECTED; return; }
        done += n;
    }
}
static void storage_get_size(IpcObject *self, IpcCall *c) { StorageFile *st = self->state; int64_t v = (int64_t)st->size; IPC_OUT(c, v); }
static void storage_write_denied(IpcObject *self, IpcCall *c) { (void)self; c->result = R_FS_INVALID_ARGUMENT; }
static void storage_flush(IpcObject *self, IpcCall *c) { (void)self; (void)c; }
static const IpcCmd storage_cmds[] = {
    {0, "Read", storage_read, IPC_IMPL}, {1, "Write", storage_write_denied, IPC_IMPL}, {2, "Flush", storage_flush, IPC_IMPL},
    {3, "SetSize", storage_write_denied, IPC_IMPL}, {4, "GetSize", storage_get_size, IPC_IMPL},
};
static IpcObject *open_romfs_storage(IpcCall *c) {
    char path[1200]; snprintf(path, sizeof path, "%s/romfs.bin", g_cfg.data_dir);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { LOG("fs", "OpenDataStorageByCurrentProcess: %s is missing", path); summary_note("romfs.bin missing"); c->result = R_FS_NOT_FOUND_DATA; return NULL; }
    struct stat sb; fstat(fd, &sb);
    StorageFile *st = calloc(1, sizeof *st); st->fd = fd; st->size = (uint64_t)sb.st_size;
    LOG("fs", "RomFS storage: %s (%llu bytes)", path, (unsigned long long)st->size);
    return IPC_OBJECT("IStorage", storage_cmds, st, storage_destroy);
}

/* ---- fsp-srv ------------------------------------------------------------------------------------------------- */
static void srv_noop(IpcObject *s, IpcCall *c) { (void)s; (void)c; }
static void srv_open_sdcard(IpcObject *s, IpcCall *c) {
    (void)s; char root[1200]; snprintf(root, sizeof root, "%s/%s/sdcard", g_cfg.save_root, g_cfg.title_id);
    ipc_out_object(c, open_host_fs(root, 0, 0, 0));
}
/* Temporary storage is erased when the application starts: the first mount in a process empties it. */
static int temporary_first_mount(const char *root) {
    static char seen[8][1300]; static unsigned nseen;
    pthread_mutex_lock(&g_meta_mu);
    int first = 1;
    for (unsigned i = 0; i < nseen; i++) if (!strcmp(seen[i], root)) first = 0;
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
    char root[1300]; save_root(space, attr, root, sizeof root);
    uint8_t type = attr[0x20];
    if (type == SAVE_TEMPORARY && temporary_first_mount(root)) { rm_rf(root, 1); LOGV("fs", "temporary storage %s emptied (first mount)", root); }
    if (!read_only) mkdirs(root);
    SaveMeta m; int64_t quota = 0;
    pthread_mutex_lock(&g_meta_mu);
    if (meta_get(root, space, attr, &m)) quota = m.extra.data_size;
    pthread_mutex_unlock(&g_meta_mu);
    if (read_only && !path_exists(root)) { c->result = R_FS_TARGET_NOT_FOUND; return; }
    ipc_out_object(c, open_host_fs(root, read_only, save_type_journaled(type), quota));
}
static void srv_open_save(IpcObject *s, IpcCall *c) { (void)s; open_save(c, 0); }
static void srv_open_save_read_only(IpcObject *s, IpcCall *c) { (void)s; open_save(c, 1); }

/* CreateSaveDataFileSystem(SaveDataAttribute, SaveDataCreationInfo {s64 size, s64 journal size, u64 block size, u64 owner, u32 flags,
 * u8 space, ...}[, SaveDataMetaInfo][, HashSalt]): the directory and its meta record. PathAlreadyExists when it was created before (the
 * SDK's EnsureSaveData treats that as success); a directory that exists without a record (opened before it was created) is adopted. */
static void srv_create_save(IpcObject *s, IpcCall *c) {
    (void)s;
    const uint8_t *attr = ipc_in(c, 0, 0x40), *ci = ipc_in(c, 0x40, 0x40);
    int64_t size, journal; uint64_t owner; uint32_t flags; memcpy(&size, ci, 8); memcpy(&journal, ci + 8, 8); memcpy(&owner, ci + 0x18, 8); memcpy(&flags, ci + 0x20, 4);
    uint8_t space = ci[0x24];
    if (size < 0 || journal < 0 || attr[0x20] > SAVE_CACHE) { c->result = R_FS_INVALID_ARGUMENT; return; }
    char root[1300]; save_root(space, attr, root, sizeof root);
    pthread_mutex_lock(&g_meta_mu);
    SaveMeta m;
    if (meta_read(root, &m) && !m.implicit) { pthread_mutex_unlock(&g_meta_mu); c->result = R_FS_PATH_EXISTS; return; }
    mkdirs(root);
    meta_init(&m, root, space, attr);                /* a record made on first use takes the creation info now */
    m.extra.owner_id = owner; m.extra.flags = flags; m.extra.data_size = size; m.extra.journal_size = journal;
    int w = meta_write(root, &m);
    pthread_mutex_unlock(&g_meta_mu);
    if (w) { c->result = errno_result(); return; }
    LOGV("fs", "created save %s (id %#llx, size %#llx, journal %#llx)", root, (unsigned long long)m.id, (unsigned long long)size, (unsigned long long)journal);
}
static void srv_delete_save_by_id(IpcObject *s, IpcCall *c, int space, uint64_t id) {
    (void)s;
    char root[1300]; SaveMeta m;
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
    int lead = 1; for (int i = 1; i < 8; i++) if (in[i]) lead = 0;
    if (lead) { *space = in[0]; *attr = in + 8; } else { *attr = in; *space = in[0x40]; }
}
static void srv_delete_save_by_attr(IpcObject *s, IpcCall *c) {
    (void)s; uint8_t space; const uint8_t *attr; space_and_attr(c, &space, &attr);
    char root[1300]; save_root(space, attr, root, sizeof root);
    pthread_mutex_lock(&g_meta_mu);
    SaveMeta m; int found = meta_get(root, space, attr, &m);
    if (found) save_delete(root);
    pthread_mutex_unlock(&g_meta_mu);
    if (!found) c->result = R_FS_TARGET_NOT_FOUND;
}
/* ExtendSaveDataFileSystem(u8 space, u64 id, s64 size, s64 journal size): sizes only grow. */
static void srv_extend_save(IpcObject *s, IpcCall *c) {
    (void)s; uint8_t space = IPC_IN(c, uint8_t, 0); uint64_t id = IPC_IN(c, uint64_t, 8); int64_t size = IPC_IN(c, int64_t, 16), journal = IPC_IN(c, int64_t, 24);
    char root[1300]; SaveMeta m;
    pthread_mutex_lock(&g_meta_mu);
    if (!find_by_id(space, id, root, sizeof root, &m)) c->result = R_FS_TARGET_NOT_FOUND;
    else if (size < m.extra.data_size || journal < m.extra.journal_size) c->result = R_FS_INVALID_ARGUMENT;
    else { m.extra.data_size = size; m.extra.journal_size = journal; if (meta_write(root, &m)) c->result = errno_result(); }
    pthread_mutex_unlock(&g_meta_mu);
}
/* Cache storage of this application by index (DeleteCacheStorage / GetCacheStorageSize): space 0 (user), type cache, no user id. */
static void cache_attr(uint16_t index, uint8_t attr[0x40]) { memset(attr, 0, 0x40); memcpy(attr, &g_program_id, 8); attr[0x20] = SAVE_CACHE; memcpy(attr + 0x22, &index, 2); }
static void srv_delete_cache(IpcObject *s, IpcCall *c) {
    (void)s; uint8_t attr[0x40]; cache_attr(IPC_IN(c, uint16_t, 0), attr);
    char root[1300]; save_root(0, attr, root, sizeof root);
    pthread_mutex_lock(&g_meta_mu);
    SaveMeta m; int found = meta_get(root, 0, attr, &m);
    if (found) save_delete(root);
    pthread_mutex_unlock(&g_meta_mu);
    if (!found) c->result = R_FS_TARGET_NOT_FOUND;
}
static void srv_cache_size(IpcObject *s, IpcCall *c) {
    (void)s; uint8_t attr[0x40]; cache_attr(IPC_IN(c, uint16_t, 0), attr);
    char root[1300]; save_root(0, attr, root, sizeof root);
    pthread_mutex_lock(&g_meta_mu);
    SaveMeta m; int found = meta_get(root, 0, attr, &m);
    pthread_mutex_unlock(&g_meta_mu);
    if (!found) { c->result = R_FS_TARGET_NOT_FOUND; return; }
    IPC_OUT(c, m.extra.data_size); IPC_OUT(c, m.extra.journal_size);
}

/* Extra data reads and (masked) writes. */
static void extra_out(IpcCall *c, const SaveMeta *m, const uint8_t *mask) {
    const IpcBuf *b = ipc_out_buf(c, 0);
    if (!b) { c->result = R_FS_INVALID_ARGUMENT; return; }
    uint8_t out[0x200]; memcpy(out, &m->extra, sizeof out);
    if (mask) for (unsigned i = 0; i < sizeof out; i++) out[i] &= mask[i];
    ipc_write_buf(c, b, out, sizeof out);
}
static const uint8_t *in_extra(const IpcCall *c, unsigned n) {
    const IpcBuf *b = ipc_in_buf(c, n);
    return b && b->size >= 0x200 ? ipc_buf_ptr(b) : NULL;
}
static void extra_write(IpcCall *c, const char *root, SaveMeta *m, const uint8_t *data, const uint8_t *mask) {
    if (!data) { c->result = R_FS_INVALID_ARGUMENT; return; }
    uint8_t *cur = (uint8_t *)&m->extra;
    for (unsigned i = 0; i < 0x200; i++) cur[i] = mask ? (uint8_t)((cur[i] & ~mask[i]) | (data[i] & mask[i])) : data[i];
    if (meta_write(root, m)) c->result = errno_result();
}
static void srv_read_extra_by_space(IpcObject *s, IpcCall *c) {
    (void)s; char root[1300]; SaveMeta m;
    pthread_mutex_lock(&g_meta_mu); int found = find_by_id(IPC_IN(c, uint8_t, 0), IPC_IN(c, uint64_t, 8), root, sizeof root, &m); pthread_mutex_unlock(&g_meta_mu);
    if (!found) { c->result = R_FS_TARGET_NOT_FOUND; return; }
    extra_out(c, &m, NULL);
}
static void srv_read_extra(IpcObject *s, IpcCall *c) {
    (void)s; char root[1300]; SaveMeta m;
    pthread_mutex_lock(&g_meta_mu); int found = find_by_id(-1, IPC_IN(c, uint64_t, 0), root, sizeof root, &m); pthread_mutex_unlock(&g_meta_mu);
    if (!found) { c->result = R_FS_TARGET_NOT_FOUND; return; }
    extra_out(c, &m, NULL);
}
/* WriteSaveDataFileSystemExtraData(WithMask)(u64 id, u8 space, buffer[, mask]) */
static void write_extra_by_id(IpcCall *c, int masked) {
    char root[1300]; SaveMeta m;
    pthread_mutex_lock(&g_meta_mu);
    if (!find_by_id(IPC_IN(c, uint8_t, 8), IPC_IN(c, uint64_t, 0), root, sizeof root, &m)) c->result = R_FS_TARGET_NOT_FOUND;
    else extra_write(c, root, &m, in_extra(c, 0), masked ? in_extra(c, 1) : NULL);
    pthread_mutex_unlock(&g_meta_mu);
}
static void srv_write_extra(IpcObject *s, IpcCall *c) { (void)s; write_extra_by_id(c, 0); }
static void srv_write_extra_mask(IpcObject *s, IpcCall *c) { (void)s; write_extra_by_id(c, 1); }
static int meta_by_attr(IpcCall *c, char *root, size_t cap, SaveMeta *m) {
    uint8_t space; const uint8_t *attr; space_and_attr(c, &space, &attr);
    save_root(space, attr, root, cap);
    pthread_mutex_lock(&g_meta_mu); int found = meta_get(root, space, attr, m); pthread_mutex_unlock(&g_meta_mu);
    if (!found) c->result = R_FS_TARGET_NOT_FOUND;
    return found;
}
static void srv_read_extra_by_attr(IpcObject *s, IpcCall *c) { (void)s; char root[1300]; SaveMeta m; if (meta_by_attr(c, root, sizeof root, &m)) extra_out(c, &m, NULL); }
static void srv_read_extra_mask_by_attr(IpcObject *s, IpcCall *c) {
    (void)s; char root[1300]; SaveMeta m;
    const uint8_t *mask = in_extra(c, 0);
    if (!mask) { c->result = R_FS_INVALID_ARGUMENT; return; }
    if (meta_by_attr(c, root, sizeof root, &m)) extra_out(c, &m, mask);
}
static void srv_write_extra_mask_by_attr(IpcObject *s, IpcCall *c) {
    (void)s; char root[1300]; SaveMeta m;
    if (!meta_by_attr(c, root, sizeof root, &m)) return;
    pthread_mutex_lock(&g_meta_mu); extra_write(c, root, &m, in_extra(c, 0), in_extra(c, 1)); pthread_mutex_unlock(&g_meta_mu);
}

/* ISaveDataInfoReader: SaveDataInfo {u64 id, u8 space, u8 type, pad[6], u128 user, u64 system id, u64 program id, s64 size, u16 index,
 * u8 rank, pad} (0x60 each), over the saves that have a meta record. */
typedef struct { uint8_t (*infos)[0x60]; unsigned n, cap, next; int space, only_cache; } InfoReader;
static int info_collect(const char *root, const SaveMeta *m, void *arg) {
    (void)root; InfoReader *r = arg; const uint8_t *a = m->extra.attr;
    if ((r->space >= 0 && m->space != r->space) || (r->only_cache && a[0x20] != SAVE_CACHE)) return 0;
    if (r->n == r->cap) { unsigned nc = r->cap ? r->cap * 2 : 16; void *p = realloc(r->infos, (size_t)nc * 0x60); if (!p) return 1; r->infos = p; r->cap = nc; }
    uint8_t *e = r->infos[r->n++]; memset(e, 0, 0x60);
    memcpy(e, &m->id, 8); e[8] = m->space; e[9] = a[0x20]; memcpy(e + 0x10, a + 8, 16); memcpy(e + 0x20, a + 0x18, 8); memcpy(e + 0x28, a, 8);
    memcpy(e + 0x30, &m->extra.data_size, 8); memcpy(e + 0x38, a + 0x22, 2); e[0x3a] = a[0x21];
    return 0;
}
static void reader_destroy(IpcObject *o) { InfoReader *r = o->state; if (r) { free(r->infos); free(r); } }
static void reader_read(IpcObject *self, IpcCall *c) {
    InfoReader *r = self->state; const IpcBuf *b = ipc_out_buf(c, 0);
    if (!b) { c->result = R_FS_INVALID_ARGUMENT; return; }
    int64_t n = 0;
    while (r->next < r->n && (uint64_t)(n + 1) * 0x60 <= b->size) {
        IpcBuf at = *b; at.addr = b->addr + (uint64_t)n * 0x60; at.size = 0x60;
        ipc_write_buf(c, &at, r->infos[r->next++], 0x60); n++;
    }
    IPC_OUT(c, n);
}
static const IpcCmd reader_cmds[] = {{0, "ReadSaveDataInfo", reader_read, IPC_IMPL}};
static void open_reader(IpcCall *c, int space, int only_cache) {
    InfoReader *r = calloc(1, sizeof *r); r->space = space; r->only_cache = only_cache;
    pthread_mutex_lock(&g_meta_mu); meta_walk(host_save_dir(), 6, info_collect, r); pthread_mutex_unlock(&g_meta_mu);
    ipc_out_object(c, IPC_OBJECT("ISaveDataInfoReader", reader_cmds, r, reader_destroy));
}
static void srv_open_reader(IpcObject *s, IpcCall *c) { (void)s; open_reader(c, -1, 0); }
static void srv_open_reader_space(IpcObject *s, IpcCall *c) { (void)s; open_reader(c, IPC_IN(c, uint8_t, 0), 0); }
static void srv_open_reader_cache(IpcObject *s, IpcCall *c) { (void)s; open_reader(c, 0, 1); }
static void srv_open_data_storage(IpcObject *s, IpcCall *c) { (void)s; IpcObject *o = open_romfs_storage(c); if (o) ipc_out_object(c, o); }
static void srv_get_log_mode(IpcObject *s, IpcCall *c) { (void)s; uint32_t v = 0; IPC_OUT(c, v); }
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
    {200, "OpenDataStorageByCurrentProcess", srv_open_data_storage, IPC_IMPL},
    {1003, "DisableAutoSaveDataCreation", srv_noop, IPC_STUB},
    {1005, "GetGlobalAccessLogMode", srv_get_log_mode, IPC_STUB},
    {1006, "OutputAccessLogToSdCard", srv_noop, IPC_STUB},
};
static IpcObject *fsp_open(void) { return IPC_OBJECT("fsp-srv", srv_cmds, NULL, NULL); }
void service_register_fs(void) { ipc_register_service("fsp-srv", fsp_open); }

/* Called when the process stops: tells which transactional save roots still hold changes the game never committed (those are rolled back
 * at the next start, exactly like on the console). */
void fs_report_uncommitted(void) {
    for (int i = 0; i < 16 && g_journals[i]; i++)
        if (g_journals[i]->dirty) LOG("fs", "save %s: changes since the last commit are discarded (%u commits this run)", g_journals[i]->live, g_journals[i]->commits);
        else if (g_journals[i]->commits) LOG("fs", "save %s: %u commits this run, nothing pending", g_journals[i]->live, g_journals[i]->commits);
}
