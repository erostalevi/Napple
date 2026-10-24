/* Host unit tests: fsp-srv save data management (fs.c, included for its static handlers): CreateSaveDataFileSystem and its meta record,
 * PathAlreadyExists, adoption of a directory opened before creation, extra data reads / masked writes by id and by attribute (both argument
 * orders), ExtendSaveDataFileSystem, free / total space of a sized save, cache storage size and deletion, the save data info readers,
 * temporary storage emptied at its first mount, read-only mounts, deletion resetting the transactional journal. */
#include "../../src/services/fs.c"
#include "host_stubs.h"
#include "../check.h"

static IpcCall call;
static uint8_t in[0x100];
static IpcCall *run(IpcObject *o, unsigned cmd, unsigned in_size) {
    memset(&call, 0, sizeof call);
    call.object = o; call.cmd = cmd; call.in = in; call.in_size = in_size;
    for (size_t i = 0; i < o->ncmds; i++) if (o->cmds[i].cmd == cmd) { o->cmds[i].fn(o, &call); return &call; }
    printf("no command %u\n", cmd); exit(2);
}
static void attr_of(uint8_t *a, uint8_t type, uint8_t uid0, uint16_t index) {
    memset(a, 0, 0x40); uint64_t pid = 0x0100000000010000ull; memcpy(a, &pid, 8); a[8] = uid0; a[0x20] = type; memcpy(a + 0x22, &index, 2);
}
static void creation(uint8_t *ci, int64_t size, int64_t journal, uint64_t owner, uint8_t space) {
    memset(ci, 0, 0x40); memcpy(ci, &size, 8); memcpy(ci + 8, &journal, 8); memcpy(ci + 0x18, &owner, 8); ci[0x24] = space;
}
static uint64_t id_of(uint8_t space, const uint8_t *attr) { char r[1300]; save_root(space, attr, r, sizeof r); return save_id_for(r); }

int main(void) {
    char base[] = "/tmp/napple-fssave-XXXXXX"; if (!mkdtemp(base)) return 2;
    g_cfg.save_root = base; snprintf(g_cfg.title_id, sizeof g_cfg.title_id, "0100000000010000"); g_program_id = 0x0100000000010000ull;
    host_thread_attach(calloc(1, 0x200));
    IpcObject *srv = fsp_open();

    /* create an account save with a size, then again */
    uint8_t attr[0x40]; attr_of(attr, SAVE_ACCOUNT, 0x11, 0);
    memcpy(in, attr, 0x40); creation(in + 0x40, 0x80000, 0x40000, 0x0100000000010000ull, 0);
    CHECK(run(srv, 22, 0x90)->result == 0, "CreateSaveDataFileSystem");
    CHECK(run(srv, 22, 0x90)->result == R_FS_PATH_EXISTS, "creating it again: PathAlreadyExists");
    uint64_t id = id_of(0, attr);

    /* extra data by id (space + id) and by id alone */
    static uint8_t extra[0x200], mask[0x200];
    memset(&call, 0, sizeof call);
    memset(in, 0, sizeof in); in[0] = 0; memcpy(in + 8, &id, 8);
    call.object = srv; call.in = in; call.in_size = 16; call.nb = 1; call.b[0] = (IpcBuf){(uintptr_t)extra, sizeof extra, 0};
    srv_read_extra_by_space(srv, &call);
    CHECK(call.result == 0 && ((SaveExtraData *)extra)->data_size == 0x80000, "ReadSaveDataFileSystemExtraDataBySaveDataSpaceId");
    in[0] = 1; srv_read_extra_by_space(srv, &call);
    CHECK(call.result == R_FS_TARGET_NOT_FOUND, "same id in another space");
    IpcCall *c;
    memset(&call, 0, sizeof call);
    memset(in, 0, sizeof in); memcpy(in, &id, 8);
    call.object = srv; call.in = in; call.in_size = 8; call.nb = 1; call.b[0] = (IpcBuf){(uintptr_t)extra, sizeof extra, 0};
    srv_read_extra(srv, &call);
    SaveExtraData *x = (SaveExtraData *)extra;
    CHECK(call.result == 0 && x->data_size == 0x80000 && x->journal_size == 0x40000 && x->owner_id == 0x0100000000010000ull && x->attr[0x20] == SAVE_ACCOUNT, "extra data holds the creation info");

    /* masked write by attribute: only the flags field */
    memset(extra, 0, sizeof extra); memset(mask, 0, sizeof mask);
    x->flags = 0x5; memset(mask + 0x50, 0xff, 4); x->data_size = 1;   /* data_size outside the mask: must stay */
    memset(&call, 0, sizeof call);
    memset(in, 0, sizeof in); in[0] = 0; memcpy(in + 8, attr, 0x40);
    call.object = srv; call.in = in; call.in_size = 0x48; call.na = 2; call.a[0] = (IpcBuf){(uintptr_t)extra, 0x200, 0}; call.a[1] = (IpcBuf){(uintptr_t)mask, 0x200, 1};
    srv_write_extra_mask_by_attr(srv, &call);
    CHECK(call.result == 0, "WriteSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute (space first)");
    static uint8_t back[0x200];
    memset(&call, 0, sizeof call);
    memcpy(in, attr, 0x40); in[0x40] = 0; memset(in + 0x41, 0, 7);              /* the other argument order: attribute first */
    call.object = srv; call.in = in; call.in_size = 0x48; call.nb = 1; call.b[0] = (IpcBuf){(uintptr_t)back, 0x200, 0};
    srv_read_extra_by_attr(srv, &call);
    SaveExtraData *y = (SaveExtraData *)back;
    CHECK(call.result == 0 && y->flags == 5 && y->data_size == 0x80000, "masked write changed only the masked field (read with attribute first)");
    memset(&call, 0, sizeof call); memset(mask, 0, sizeof mask); memset(mask + 0x58, 0xff, 8);
    in[0] = 0; memset(in + 1, 0, 7); memcpy(in + 8, attr, 0x40);
    call.object = srv; call.in = in; call.in_size = 0x48; call.na = 1; call.a[0] = (IpcBuf){(uintptr_t)mask, 0x200, 0}; call.nb = 1; call.b[0] = (IpcBuf){(uintptr_t)back, 0x200, 0};
    srv_read_extra_mask_by_attr(srv, &call);
    CHECK(call.result == 0 && y->data_size == 0x80000 && y->flags == 0 && y->owner_id == 0, "masked read returns only the masked bytes");

    /* sizes: total = created size, free = size minus blocks in use */
    memset(in, 0, sizeof in); in[0] = 0; memcpy(in + 8, attr, 0x40);
    c = run(srv, 51, 0x48);
    CHECK(c->result == 0 && c->nout_objects == 1, "OpenSaveDataFileSystem");
    IpcObject *fsobj = c->out_objects[0];
    HostFs *hf = fsobj->state;
    char fpath[1400]; snprintf(fpath, sizeof fpath, "%s/a.bin", hf->root);
    FILE *f = fopen(fpath, "wb"); static uint8_t junk[0x5000]; fwrite(junk, 1, sizeof junk, f); fclose(f);
    int64_t total, freev;
    c = run(fsobj, 12, 0); memcpy(&total, c->out, 8);
    c = run(fsobj, 11, 0); memcpy(&freev, c->out, 8);
    CHECK(total == 0x80000 && freev == 0x80000 - 0x8000, "GetTotalSpaceSize / GetFreeSpaceSize of a sized save");
    /* extend */
    memset(in, 0, sizeof in); in[0] = 0; memcpy(in + 8, &id, 8); int64_t ns = 0x100000, nj = 0x40000; memcpy(in + 16, &ns, 8); memcpy(in + 24, &nj, 8);
    CHECK(run(srv, 32, 32)->result == 0, "ExtendSaveDataFileSystem");
    ns = 0x1000; memcpy(in + 16, &ns, 8);
    CHECK(run(srv, 32, 32)->result == R_FS_INVALID_ARGUMENT, "sizes never shrink");
    memset(in, 0, sizeof in); in[0] = 0; memcpy(in + 8, attr, 0x40);
    c = run(srv, 51, 0x48); fsobj = c->out_objects[0];
    c = run(fsobj, 12, 0); memcpy(&total, c->out, 8);
    CHECK(total == 0x100000, "a new mount sees the extended size");

    /* a save opened before it was created is adopted */
    uint8_t dev[0x40]; attr_of(dev, SAVE_DEVICE, 0, 0);
    memset(in, 0, sizeof in); in[0] = 0; memcpy(in + 8, dev, 0x40);
    CHECK(run(srv, 51, 0x48)->result == 0, "open a device save that was never created");
    memcpy(in, dev, 0x40); creation(in + 0x40, 0x20000, 0x10000, 0, 0);
    CHECK(run(srv, 22, 0x90)->result == 0, "creating it afterwards adopts the directory");
    CHECK(run(srv, 22, 0x90)->result == R_FS_PATH_EXISTS, "and then it exists");

    /* cache storage */
    uint8_t cache[0x40]; attr_of(cache, SAVE_CACHE, 0, 2);
    memcpy(in, cache, 0x40); creation(in + 0x40, 0x30000, 0x8000, 0, 0);
    CHECK(run(srv, 22, 0x90)->result == 0, "create cache storage index 2");
    memset(in, 0, sizeof in); uint16_t idx = 2; memcpy(in, &idx, 2);
    c = run(srv, 34, 2); int64_t cs[2]; memcpy(cs, c->out, 16);
    CHECK(c->result == 0 && cs[0] == 0x30000 && cs[1] == 0x8000, "GetCacheStorageSize");
    idx = 3; memcpy(in, &idx, 2);
    CHECK(run(srv, 34, 2)->result == R_FS_TARGET_NOT_FOUND, "cache index that does not exist");

    /* info readers */
    c = run(srv, 60, 0); IpcObject *rd = c->out_objects[0];
    static uint8_t infos[0x60 * 8];
    memset(&call, 0, sizeof call); call.object = rd; call.nb = 1; call.b[0] = (IpcBuf){(uintptr_t)infos, sizeof infos, 0};
    reader_read(rd, &call); int64_t n; memcpy(&n, call.out, 8);
    CHECK(n == 3, "OpenSaveDataInfoReader lists the three saves");
    int seen_cache = 0; for (int i = 0; i < n; i++) if (infos[i * 0x60 + 9] == SAVE_CACHE) { int64_t sz; memcpy(&sz, infos + i * 0x60 + 0x30, 8); uint16_t ix; memcpy(&ix, infos + i * 0x60 + 0x38, 2); seen_cache = sz == 0x30000 && ix == 2; }
    CHECK(seen_cache, "cache entry: size and index");
    c = run(srv, 62, 0); rd = c->out_objects[0];
    memset(&call, 0, sizeof call); call.object = rd; call.nb = 1; call.b[0] = (IpcBuf){(uintptr_t)infos, sizeof infos, 0};
    reader_read(rd, &call); memcpy(&n, call.out, 8);
    CHECK(n == 1 && infos[9] == SAVE_CACHE, "OnlyCacheStorage reader");
    memset(&call, 0, sizeof call); call.object = rd; call.nb = 1; call.b[0] = (IpcBuf){(uintptr_t)infos, sizeof infos, 0};
    reader_read(rd, &call); memcpy(&n, call.out, 8);
    CHECK(n == 0, "reader exhausted");
    idx = 2; memset(in, 0, sizeof in); memcpy(in, &idx, 2);
    CHECK(run(srv, 33, 2)->result == 0 && run(srv, 34, 2)->result == R_FS_TARGET_NOT_FOUND, "DeleteCacheStorage");

    /* temporary storage: emptied at its first mount in a process only */
    uint8_t tmp[0x40]; attr_of(tmp, SAVE_TEMPORARY, 0, 0);
    char troot[1300]; save_root(0, tmp, troot, sizeof troot); mkdirs(troot);
    char tf[1400]; snprintf(tf, sizeof tf, "%s/left.bin", troot); f = fopen(tf, "w"); fputs("x", f); fclose(f);
    memset(in, 0, sizeof in); memcpy(in + 8, tmp, 0x40);
    run(srv, 51, 0x48);
    CHECK(!path_exists(tf), "temporary storage emptied at the first mount");
    f = fopen(tf, "w"); fputs("y", f); fclose(f);
    run(srv, 51, 0x48);
    CHECK(path_exists(tf), "a second mount in the same process keeps it");

    /* read-only mounts */
    uint8_t other[0x40]; attr_of(other, SAVE_ACCOUNT, 0x22, 0);
    memset(in, 0, sizeof in); memcpy(in + 8, other, 0x40);
    CHECK(run(srv, 53, 0x48)->result == R_FS_TARGET_NOT_FOUND, "read-only mount of a missing save");
    memcpy(in + 8, attr, 0x40);
    c = run(srv, 53, 0x48);
    CHECK(c->result == 0 && ((HostFs *)c->out_objects[0]->state)->read_only, "read-only mount is read only");

    /* delete by id: the journal starts over */
    memset(in, 0, sizeof in); memcpy(in, &id, 8);
    CHECK(run(srv, 21, 8)->result == 0, "DeleteSaveDataFileSystem");
    char aroot[1300]; save_root(0, attr, aroot, sizeof aroot);
    CHECK(!path_exists(aroot) && run(srv, 21, 8)->result == R_FS_TARGET_NOT_FOUND, "save and record gone");
    SaveJournal *j = journal_get(aroot);
    CHECK(j && path_exists(aroot) && !path_exists(j->committed), "recreated root starts with a fresh journal");
    memset(in, 0, sizeof in); in[0] = 0; memcpy(in + 8, dev, 0x40);
    CHECK(run(srv, 28, 0x48)->result == 0, "DeleteSaveDataFileSystemBySaveDataAttribute");

    /* the review fixes in the commit path: a failed snapshot keeps the root dirty */
    journal_mark_dirty(j); chmod(aroot, 0); int r = journal_commit(j); chmod(aroot, 0755);
    if (getuid() != 0) CHECK(r != 0 && j->dirty, "failed commit leaves the root dirty");
    CHECK(journal_commit(j) == 0 && !j->dirty, "commit clears it");

    char cmd[1200]; snprintf(cmd, sizeof cmd, "rm -rf %s", base); if (system(cmd)) printf("cleanup failed\n");
    printf("{\"test\":\"fs_savedata\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
