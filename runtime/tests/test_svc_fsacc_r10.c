/* Unit tests (ipc-depth-95 r10, part fsacc): the fsp-srv / IFileSystem rows that were documented-default stubs and now do real work: QueryEntry (15) and
 * GetFileSystemAttribute (16) on the host file systems; OpenFileSystem (0, removed), the access-checked commands (31, 80-83, 85-88, 110, 606, 610, 618)
 * against main.npdm's FS permission bits, CreateSaveDataFileSystemWithCreationInfo2 (37) with its meta file, GetAndClearErrorInfo (800) and
 * SetSaveDataSize (1001). fs.c is #included. */
#include "../src/services/fs.c"
#include "svc_test.h"
#include <unistd.h>

#define APP 0x0100000000010000ULL

static IpcObject *g_srv;
static IpcCall call(IpcObject *o, unsigned cmd, const void *in, uint32_t n) {
    IpcCall c;
    svc_call(o, cmd, in, n, &c);
    return c;
}
static IpcCall call_path(IpcObject *o, unsigned cmd, const char *path, const void *in, uint32_t n, void *out, uint64_t out_n) {
    IpcCall c;
    memset(&c, 0, sizeof c);
    c.in = in;
    c.in_size = n;
    svc_set_in_buf(&c, path, strlen(path) + 1);
    if (out) svc_set_out_buf(&c, out, out_n);
    svc_call_c(o, cmd, &c);
    return c;
}
static void drop(IpcObject *o) {
    if (o->destroy) o->destroy(o);
    free(o);
}
static void put(const char *path, const char *s) {
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs(s, f);
        fclose(f);
    }
}
static IpcObject *open_sd(void) {
    IpcCall c = call(g_srv, 18, NULL, 0);
    CHECK(c.result == 0 && c.nout_objects == 1, "18 OpenSdCardFileSystem");
    return c.nout_objects ? c.out_objects[0] : NULL;
}
static uint32_t query(IpcObject *fs, uint32_t id, const char *path, uint8_t *out) {
    uint8_t o = 0xEE;
    IpcCall c = call_path(fs, 15, path, &id, 4, &o, 1);
    if (out) *out = o;
    return c.result;
}

/* 15 QueryEntry / 16 GetFileSystemAttribute */
static void t_query_entry(void) {
    IpcObject *sd = open_sd();
    if (!sd) return;
    const HostFs *hf = sd->state;
    char p[1400];
    snprintf(p, sizeof p, "%s/big", hf->root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/big/00", hf->root);
    put(p, "abc");
    snprintf(p, sizeof p, "%s/big/01", hf->root);
    put(p, "defg");
    CHECK(query(sd, 0, "/big", NULL) == 0, "15 QueryEntry(SetConcatenationFileAttribute) on an SD card directory of chunk files");
    IpcCall t = call_path(sd, 7, "/big", NULL, 0, NULL, 0);
    CHECK(t.result == 0 && SVC_OUT(&t, uint32_t, 0) == 1, "...the directory is now a file (GetEntryType)");
    uint32_t mode = 1;
    IpcCall o = call_path(sd, 8, "/big", &mode, 4, NULL, 0);
    CHECK(o.result == 0 && o.nout_objects == 1, "...which opens as a file");
    if (o.nout_objects) {
        IpcCall g = call(o.out_objects[0], 4, NULL, 0);
        CHECK(g.result == 0 && SVC_OUT(&g, int64_t, 0) == 7, "...of the chunks' joined size");
        uint8_t in[24] = {0}, back[16] = {0};
        int64_t size = 7;
        memcpy(in + 16, &size, 8);
        IpcCall r;
        memset(&r, 0, sizeof r);
        r.in = in;
        r.in_size = sizeof in;
        svc_set_out_buf(&r, back, sizeof back);
        svc_call_c(o.out_objects[0], 0, &r);
        CHECK(r.result == 0 && !memcmp(back, "abcdefg", 7), "...holding the chunks in order");
        drop(o.out_objects[0]);
    }
    CHECK(query(sd, 0, "/big", NULL) == 0, "...setting the attribute on what is already a file changes nothing");
    snprintf(p, sizeof p, "%s/empty", hf->root);
    mkdir(p, 0755);
    CHECK(query(sd, 0, "/empty", NULL) == 0, "an empty directory becomes an empty file");
    t = call_path(sd, 7, "/empty", NULL, 0, NULL, 0);
    CHECK(t.result == 0 && SVC_OUT(&t, uint32_t, 0) == 1, "...GetEntryType says file");
    snprintf(p, sizeof p, "%s/mixed", hf->root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/mixed/notes.txt", hf->root);
    put(p, "x");
    CHECK(query(sd, 0, "/mixed", NULL) == R_FS_INVALID_ARGUMENT, "a directory that is not a run of chunk files is refused");
    snprintf(p, sizeof p, "%s/gap", hf->root);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/gap/01", hf->root);
    put(p, "x");
    CHECK(query(sd, 0, "/gap", NULL) == R_FS_INVALID_ARGUMENT, "...as is one with a missing chunk 00");
    CHECK(query(sd, 0, "/nothing", NULL) == R_FS_PATH_NOT_FOUND, "a missing path is PathNotFound");
    uint8_t v = 0xEE;
    CHECK(query(sd, 2, "/", &v) == 0 && v == 0, "query 2 IsValidSignedSystemPartitionOnSdCard: none on this SD card");
    CHECK(query(sd, 1, "/", NULL) == R_FS_UNSUPPORTED_OPERATION, "an unknown query id is UnsupportedOperation");

    uint8_t attr[0x40] = {0};
    uint64_t prog = APP;
    memcpy(attr, &prog, 8);
    attr[0x20] = SAVE_DEVICE;
    uint8_t in[0x48] = {0};
    in[0] = 1;
    memcpy(in + 8, attr, 0x40);
    IpcCall sv = call(g_srv, 51, in, sizeof in);
    CHECK(sv.result == 0 && sv.nout_objects == 1, "a device save opens");
    if (sv.nout_objects) {
        CHECK(query(sv.out_objects[0], 0, "/", NULL) == R_FS_UNSUPPORTED_OPERATION, "QueryEntry on save data: UnsupportedOperation");
        IpcCall a = call(sv.out_objects[0], 16, NULL, 0);
        CHECK(a.result == 0 && a.out_size == 0xC0, "16 GetFileSystemAttribute: a 0xC0-byte FileSystemAttribute");
        int32_t lim[4];
        memcpy(lim, a.out + 0x28, sizeof lim);
        CHECK(a.out[0] && a.out[1] && a.out[2] && a.out[3] && !a.out[4] && !a.out[12], "...the four byte limits are set, no UTF-16 limit");
        CHECK(lim[0] == NAME_MAX && lim[1] == NAME_MAX && lim[2] == 0x300 && lim[3] == 0x300, "...entry names NAME_MAX, paths 0x300");
        drop(sv.out_objects[0]);
    }
    drop(sd);
}

/* 0, and the access checks against main.npdm's FS permission bits */
static void t_access(void) {
    CHECK(call(g_srv, 0, NULL, 0).result == IPC_R_UNKNOWN_COMMAND, "0 OpenFileSystem: removed in 2.0.0, unknown command");
    uint32_t gc[2] = {0, 1};
    uint32_t cs = 1;
    uint8_t rid[16] = {0};
    memcpy(rid, &(uint64_t){APP}, 8);
    uint64_t other = 0x0100000000020000ULL;
    g_npdm.valid = 1;
    for (int round = 0; round < 2; round++) {
        g_npdm.fs_perms = round ? (3ull << 62) : 0; /* Debug | FullPermission count only in debug mode */
        const char *why = round ? " (Debug/FullPermission bits alone)" : "";
        CHECK(call(g_srv, 31, gc, sizeof gc).result == R_FS_PERMISSION_DENIED, "31 OpenGameCardFileSystem: an application is denied%s", why);
        CHECK(call(g_srv, 81, NULL, 0).result == R_FS_PERMISSION_DENIED, "81 OpenSaveDataTransferManager denied%s", why);
        CHECK(call(g_srv, 82, NULL, 0).result == R_FS_PERMISSION_DENIED, "82 ...Version2 denied%s", why);
        CHECK(call(g_srv, 85, NULL, 0).result == R_FS_PERMISSION_DENIED, "85 ...ForSaveDataRepair denied%s", why);
        CHECK(call(g_srv, 86, NULL, 0).result == R_FS_PERMISSION_DENIED, "86 OpenSaveDataMover denied%s", why);
        CHECK(call(g_srv, 87, NULL, 0).result == R_FS_PERMISSION_DENIED, "87 ...ForRepair denied%s", why);
        CHECK(call(g_srv, 88, NULL, 0).result == R_FS_PERMISSION_DENIED, "88 ...WithPolicy denied%s", why);
        CHECK(call(g_srv, 83, &other, 8).result == R_FS_PERMISSION_DENIED, "83 OpenSaveDataTransferProhibiter denied%s", why);
        CHECK(call(g_srv, 110, &cs, 4).result == R_FS_PERMISSION_DENIED, "110 OpenContentStorageFileSystem denied%s", why);
        CHECK(call(g_srv, 606, rid, 9).result == R_FS_PERMISSION_DENIED, "606 GetRightsId denied%s", why);
        CHECK(call_path(g_srv, 610, "@User:/x.nca", NULL, 0, NULL, 0).result == R_FS_PERMISSION_DENIED, "610 GetRightsIdAndKeyGenerationByPath denied%s", why);
        CHECK(call_path(g_srv, 618, "@User:/x.nca", NULL, 0, NULL, 0).result == R_FS_PERMISSION_DENIED, "618 GetProgramId denied%s", why);
        uint8_t meta[0x48] = {0};
        meta[4] = 1;
        CHECK(call(g_srv, 80, meta, sizeof meta).result == R_FS_PERMISSION_DENIED, "80 OpenSaveDataMetaFile denied%s", why);
    }
    /* the permitted paths */
    g_npdm.fs_perms = 1ull << 4; /* GameCard */
    CHECK(call(g_srv, 31, gc, sizeof gc).result == R_FS_GAME_CARD_NOT_INSERTED, "31 with GameCard: no card is inserted");
    gc[1] = 4;
    CHECK(call(g_srv, 31, gc, sizeof gc).result == R_FS_INVALID_ARGUMENT, "...an unknown partition is refused");
    g_npdm.fs_perms = 1ull << 30; /* SaveDataTransfer */
    CHECK(call(g_srv, 81, NULL, 0).result == R_FS_NOT_IMPLEMENTED, "81 with SaveDataTransfer: no peer console or transfer server");
    CHECK(call(g_srv, 82, NULL, 0).result == R_FS_PERMISSION_DENIED, "...82 needs SaveDataTransferVersion2, not SaveDataTransfer");
    g_npdm.fs_perms = 1ull << 33; /* SaveDataTransferVersion2 */
    CHECK(call(g_srv, 82, NULL, 0).result == R_FS_NOT_IMPLEMENTED && call(g_srv, 85, NULL, 0).result == R_FS_NOT_IMPLEMENTED &&
              call(g_srv, 88, NULL, 0).result == R_FS_NOT_IMPLEMENTED,
          "82 / 85 / 88 with SaveDataTransferVersion2: NotImplemented");
    g_npdm.fs_perms = 1ull << 36;
    CHECK(call(g_srv, 86, NULL, 0).result == R_FS_NOT_IMPLEMENTED, "86 with MoveCacheStorage: NotImplemented");
    g_npdm.fs_perms = 1ull << 5;
    CHECK(call(g_srv, 87, NULL, 0).result == R_FS_NOT_IMPLEMENTED, "87 with SaveDataManagement: NotImplemented");
    g_npdm.fs_perms = 1ull << 11; /* ContentManager */
    IpcCall c = call(g_srv, 110, &cs, 4);
    CHECK(c.result == 0 && c.nout_objects == 1, "110 with ContentManager: the user content storage opens");
    if (c.nout_objects) {
        IpcCall fs = call(c.out_objects[0], 12, NULL, 0);
        CHECK(fs.result == 0, "...as an IFileSystem");
        uint64_t n = 0;
        IpcCall d = call_path(c.out_objects[0], 9, "/", &(uint32_t){3}, 4, NULL, 0);
        if (d.nout_objects) {
            IpcCall cnt = call(d.out_objects[0], 1, NULL, 0);
            n = SVC_OUT(&cnt, uint64_t, 0);
            drop(d.out_objects[0]);
        }
        CHECK(d.result == 0 && n == 0, "...holding no NCA files");
        drop(c.out_objects[0]);
    }
    cs = 4;
    CHECK(call(g_srv, 110, &cs, 4).result == R_FS_INVALID_ENUM, "...an unknown ContentStorageId is 0x2EE202");
    g_npdm.fs_perms = 1ull << 27; /* GetRightsId */
    CHECK(call(g_srv, 606, rid, 9).result == R_FS_TARGET_NOT_FOUND, "606 with GetRightsId: no installed NCA for the program");
    CHECK(call_path(g_srv, 610, "@User:/x.nca", NULL, 0, NULL, 0).result == R_FS_PATH_NOT_FOUND, "610: the content path does not exist");
    CHECK(call_path(g_srv, 618, "@User:/x.nca", NULL, 0, NULL, 0).result == R_FS_PATH_NOT_FOUND, "618: the content path does not exist");

    /* 83: own saves with CreateOwnSaveData, anyone's with SaveDataTransferVersion2 */
    g_npdm.fs_perms = 1ull << 35;
    uint64_t own = APP;
    CHECK(call(g_srv, 83, &other, 8).result == R_FS_PERMISSION_DENIED, "83 with CreateOwnSaveData: another application's saves are denied");
    IpcCall p1 = call(g_srv, 83, &own, 8), p2 = call(g_srv, 83, &own, 8);
    CHECK(p1.result == 0 && p1.nout_objects == 1 && p2.nout_objects == 1, "...the caller's own saves give an ISaveDataTransferProhibiter");
    CHECK(fs_transfer_prohibited(APP) == 2, "...two live prohibiters for the application");
    if (p1.nout_objects) {
        CHECK(svc_find(p1.out_objects[0], 0) == NULL && p1.out_objects[0]->ncmds == 1, "...the interface has no commands (and is not a tolerant stub)");
        drop(p1.out_objects[0]);
    }
    CHECK(fs_transfer_prohibited(APP) == 1, "...closing one leaves one");
    if (p2.nout_objects) drop(p2.out_objects[0]);
    CHECK(fs_transfer_prohibited(APP) == 0, "...closing both lifts the prohibition");
    g_npdm.fs_perms = 1ull << 33;
    IpcCall p3 = call(g_srv, 83, &other, 8);
    CHECK(p3.result == 0 && p3.nout_objects == 1 && fs_transfer_prohibited(other) == 1, "...with SaveDataTransferVersion2 any application's saves");
    if (p3.nout_objects) drop(p3.out_objects[0]);
    g_npdm.valid = 0;
    g_npdm.fs_perms = 0;
}

/* 37 + 22 meta info + 80 */
static void t_create_info2_and_meta(void) {
    uint8_t info[0x200] = {0};
    uint32_t version = 0x10000;
    memcpy(info, &version, 4);
    uint8_t *attr = info + 8;
    memcpy(attr, &(uint64_t){APP}, 8);
    uint8_t uid[16] = {9, 8, 7, 6, 5, 4, 3, 2, 1};
    memcpy(attr + 8, uid, 16);
    attr[0x20] = SAVE_ACCOUNT;
    int64_t size = 0x800000, journal = 0x200000;
    uint32_t meta_size = 0x40060;
    memcpy(info + 0x48, &size, 8);
    memcpy(info + 0x50, &journal, 8);
    memcpy(info + 0x60, &(uint64_t){APP}, 8);
    info[0x6C] = 1;
    info[0x94] = 1; /* Thumbnail */
    memcpy(info + 0x98, &meta_size, 4);
    IpcCall c;
    memset(&c, 0, sizeof c);
    svc_set_in_buf(&c, info, sizeof info);
    svc_call_c(g_srv, 37, &c);
    CHECK(c.result == 0, "37 CreateSaveDataFileSystemWithCreationInfo2 creates the save (result %#x)", c.result);
    char root[1300];
    save_root(1, attr, root, sizeof root);
    SaveMeta m;
    CHECK(meta_read(root, &m) && !m.implicit && m.extra.data_size == size && m.extra.journal_size == journal && m.extra.owner_id == APP,
          "...with the sizes and owner of the creation info");
    memset(&c, 0, sizeof c);
    svc_set_in_buf(&c, info, sizeof info);
    svc_call_c(g_srv, 37, &c);
    CHECK(c.result == R_FS_PATH_EXISTS, "...creating it again: PathAlreadyExists");
    memset(&c, 0, sizeof c);
    svc_set_in_buf(&c, info, 0x100);
    svc_call_c(g_srv, 37, &c);
    CHECK(c.result == R_FS_INVALID_ARGUMENT, "...a short SaveDataCreationInfo2 is refused");

    uint8_t in[0x48] = {0};
    in[0] = 1;
    uint32_t type = 1;
    memcpy(in + 4, &type, 4);
    memcpy(in + 8, attr, 0x40);
    IpcCall f = call(g_srv, 80, in, sizeof in);
    CHECK(f.result == 0 && f.nout_objects == 1, "80 OpenSaveDataMetaFile: the thumbnail meta file");
    if (f.nout_objects) {
        IpcCall g = call(f.out_objects[0], 4, NULL, 0);
        CHECK(g.result == 0 && SVC_OUT(&g, int64_t, 0) == meta_size, "...of the meta size given at creation");
        drop(f.out_objects[0]);
    }
    type = 2;
    memcpy(in + 4, &type, 4);
    CHECK(call(g_srv, 80, in, sizeof in).result == R_FS_PATH_NOT_FOUND, "...no ExtensionContext meta file was created");
    type = 3;
    memcpy(in + 4, &type, 4);
    CHECK(call(g_srv, 80, in, sizeof in).result == R_FS_INVALID_ARGUMENT, "...an unknown meta type is refused");
    type = 1;
    memcpy(in + 4, &type, 4);
    in[8 + 0x20] = SAVE_BCAT;
    CHECK(call(g_srv, 80, in, sizeof in).result == R_FS_TARGET_NOT_FOUND, "...a save that does not exist: TargetNotFound");

    /* 22 with a SaveDataMetaInfo after the creation info */
    uint8_t ci[0x90] = {0};
    memcpy(ci, &(uint64_t){APP}, 8);
    ci[0x20] = SAVE_DEVICE;
    memcpy(ci + 0x40, &size, 8);
    ci[0x40 + 0x24] = 1;
    uint32_t ext = 0x20;
    memcpy(ci + 0x80, &ext, 4);
    ci[0x84] = 2;
    CHECK(call(g_srv, 22, ci, sizeof ci).result == 0, "22 CreateSaveDataFileSystem with an ExtensionContext meta info");
    memcpy(in + 8, ci, 0x40);
    type = 2;
    memcpy(in + 4, &type, 4);
    f = call(g_srv, 80, in, sizeof in);
    CHECK(f.result == 0 && f.nout_objects == 1, "...80 opens its ExtensionContext meta file");
    if (f.nout_objects) {
        uint8_t w[40] = {0};
        int64_t wsize = 4;
        memcpy(w + 16, &wsize, 8);
        IpcCall wr;
        memset(&wr, 0, sizeof wr);
        wr.in = w;
        wr.in_size = sizeof w;
        svc_set_in_buf(&wr, "META", 4);
        svc_call_c(f.out_objects[0], 1, &wr);
        CHECK(wr.result == 0, "...which is writable");
        drop(f.out_objects[0]);
    }
    /* deleting the save removes its meta file */
    char mp[1400], droot[1300];
    save_root(1, ci, droot, sizeof droot);
    meta_file_path(droot, 2, mp, sizeof mp);
    CHECK(access(mp, F_OK) == 0, "the meta file sits next to the save");
    uint8_t del[0x48] = {0};
    del[0] = 1;
    memcpy(del + 8, ci, 0x40);
    CHECK(call(g_srv, 28, del, sizeof del).result == 0 && access(mp, F_OK) != 0, "28 DeleteSaveDataFileSystemBySaveDataAttribute removes the meta file too");
}

/* 800 GetAndClearErrorInfo, 1001 SetSaveDataSize */
static uint32_t count_saves(void) {
    uint32_t n = 0;
    meta_walk(host_save_dir(), 6, count_save_cb, &n);
    return n;
}
static void t_error_info_and_sizes(void) {
    IpcCall c = call(g_srv, 800, NULL, 0);
    CHECK(c.result == 0 && c.out_size == 0x80, "800 GetAndClearErrorInfo: a 0x80-byte FileSystemProxyErrorInfo");
    uint32_t saves;
    memcpy(&saves, c.out + 0x2C, 4);
    CHECK(saves == count_saves() && saves >= 1, "...save_data_index_count is the number of save data (%u)", saves);
    CHECK(!memcmp(c.out, (uint8_t[0x28]){0}, 0x28), "...no RomFS remount and no FAT error ever happens here");
    IpcObject *sd = open_sd();
    if (sd) {
        uint32_t mode = 1;
        IpcCall f1 = call_path(sd, 8, "/big", &mode, 4, NULL, 0), f2 = call_path(sd, 8, "/empty", &mode, 4, NULL, 0);
        IpcCall d1 = call_path(sd, 9, "/", &(uint32_t){3}, 4, NULL, 0);
        CHECK(f1.nout_objects == 1 && f2.nout_objects == 1 && d1.nout_objects == 1, "two SD card files and a directory are open");
        if (f2.nout_objects) drop(f2.out_objects[0]);
        uint8_t buf[0x80] = {0};
        IpcCall b;
        memset(&b, 0, sizeof b);
        svc_set_out_buf(&b, buf, sizeof buf);
        svc_call_c(g_srv, 800, &b);
        uint16_t pk[2];
        memcpy(pk, buf + 0x38, 4);
        CHECK(b.result == 0 && b.out_size == 0 && pk[0] == 2 && pk[1] == 1, "...the 21.0.0+ buffer form reports the SD card peaks 2 files / 1 directory");
        c = call(g_srv, 800, NULL, 0);
        memcpy(pk, c.out + 0x38, 4);
        CHECK(pk[0] == 1 && pk[1] == 1, "...cleared to what is still open (1 file, 1 directory)");
        if (f1.nout_objects) drop(f1.out_objects[0]);
        if (d1.nout_objects) drop(d1.out_objects[0]);
        call(g_srv, 800, NULL, 0);
        c = call(g_srv, 800, NULL, 0);
        memcpy(pk, c.out + 0x38, 4);
        CHECK(pk[0] == 0 && pk[1] == 0, "...and to nothing once everything is closed");
        drop(sd);
    }

    FspSession *s = g_srv->state;
    CHECK(s && s->save_size == 0x2000000 && s->save_journal_size == 0x1000000, "a new fsp-srv session starts at 32 MiB / 16 MiB");
    int64_t v[2] = {0x4000000, 0x800000};
    CHECK(call(g_srv, 1001, v, sizeof v).result == 0 && s->save_size == 0x4000000 && s->save_journal_size == 0x800000, "1001 SetSaveDataSize stores both");
    IpcObject *other = fsp_open();
    CHECK(((FspSession *)other->state)->save_size == 0x2000000, "...per session");
    drop(other);
    CHECK(call(g_srv, 1001, v, 8).result == R_FS_INVALID_ARGUMENT, "...a short request is refused");
}

int main(void) {
    char dir[256];
    snprintf(dir, sizeof dir, "%s/svc_fsacc_r10.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(dir)) {
        printf("mkdtemp failed\n");
        return 1;
    }
    static char data[300], saves[300];
    snprintf(data, sizeof data, "%s/data", dir);
    snprintf(saves, sizeof saves, "%s/saves", dir);
    mkdir(data, 0755);
    mkdir(saves, 0755);
    g_cfg.data_dir = data;
    g_cfg.save_root = saves;
    snprintf(g_cfg.title_id, sizeof g_cfg.title_id, "0100000000010000");
    g_program_id = APP;
    g_srv = fsp_open();

    t_query_entry();
    t_access();
    t_create_info2_and_meta();
    t_error_info_and_sizes();

    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) printf("cleanup failed\n");
    printf("{\"test\":\"svc_fsacc_r10\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
