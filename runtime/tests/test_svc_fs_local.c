/* Unit tests: the fsp-srv rows that used to be documented-default stubs and now do real work (ipc-local-stubs, 2026-11-01-r3):
 * RomFS as an IFileSystem (2, 9, 10, 7), content file systems (Control, Manual), data storages by program id (201, 205), save lookups with a filter
 * (67, 68), the internal storage (64), commit ids (616), free space (612), SD card state (27, 620, 630, 631), access log state (1004, 1011, 1016),
 * image / custom storage directories (100, 130), the system data update notifier (510, 511), and OperateRange on files and storages. fs.c is #included. */
#include "../src/services/fs.c"
#include "svc_test.h"
#include "romfs_fixture.h"
#include <unistd.h>

#define APP 0x0100000000010000ULL

static void put(const char *path, const void *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(p, 1, n, f);
        fclose(f);
    }
}
static IpcObject *g_srv;
static IpcCall call(IpcObject *o, unsigned cmd, const void *in, uint32_t n) {
    IpcCall c;
    svc_call(o, cmd, in, n, &c);
    return c;
}
/* Calls with one path buffer (and optional raw input). */
static IpcCall call_path(IpcObject *o, unsigned cmd, const char *path, const void *in, uint32_t n) {
    IpcCall c;
    memset(&c, 0, sizeof c);
    c.in = in;
    c.in_size = n;
    svc_set_in_buf(&c, path, strlen(path) + 1);
    svc_call_c(o, cmd, &c);
    return c;
}
static int entry_type(IpcObject *fs, const char *path, uint32_t *type) {
    IpcCall c = call_path(fs, 7, path, NULL, 0);
    if (c.result) return (int)c.result;
    *type = SVC_OUT(&c, uint32_t, 0);
    return 0;
}
static uint64_t rd_file(IpcObject *f, int64_t off, int64_t size, void *buf, size_t cap, uint32_t *result) {
    uint8_t in[24] = {0};
    memcpy(in + 8, &off, 8);
    memcpy(in + 16, &size, 8);
    IpcCall c;
    memset(&c, 0, sizeof c);
    c.in = in;
    c.in_size = sizeof in;
    svc_set_out_buf(&c, buf, cap);
    svc_call_c(f, 0, &c);
    if (result) *result = c.result;
    return c.result ? 0 : SVC_OUT(&c, uint64_t, 0);
}
static uint32_t operate(IpcObject *o, uint32_t op, int64_t off, int64_t size, uint8_t info[0x40]) {
    uint8_t in[24] = {0};
    memcpy(in, &op, 4);
    memcpy(in + 8, &off, 8);
    memcpy(in + 16, &size, 8);
    IpcCall c = call(o, 5, in, sizeof in);
    if (!c.result && info && c.out_size >= 0x40) memcpy(info, c.out, 0x40);
    return c.result;
}

static void t_romfs_file_system(void) {
    IpcCall c = call(g_srv, 2, NULL, 0);
    CHECK(c.result == 0 && c.nout_objects == 1, "2 OpenDataFileSystemByCurrentProcess opens the RomFS (result %#x)", c.result);
    if (c.nout_objects != 1) return;
    IpcObject *fs = c.out_objects[0];
    uint32_t t = 9;
    CHECK(entry_type(fs, "/a.txt", &t) == 0 && t == 1, "a.txt is a file");
    CHECK(entry_type(fs, "/dir", &t) == 0 && t == 0, "dir is a directory");
    CHECK(entry_type(fs, "/dir/sub/c.txt", &t) == 0 && t == 1, "nested file");
    CHECK(entry_type(fs, "/", &t) == 0 && t == 0, "the root is a directory");
    CHECK(entry_type(fs, "/nope", &t) == (int)R_FS_PATH_NOT_FOUND, "missing path");
    CHECK(entry_type(fs, "/dir/../a.txt", &t) == (int)R_FS_INVALID_ARGUMENT, "'..' is rejected");

    uint32_t mode = 1;
    c = call_path(fs, 8, "/dir/b.bin", &mode, 4);
    CHECK(c.result == 0 && c.nout_objects == 1, "OpenFile");
    if (c.nout_objects == 1) {
        IpcObject *f = c.out_objects[0];
        IpcCall g = call(f, 4, NULL, 0);
        CHECK(g.result == 0 && SVC_OUT(&g, int64_t, 0) == 4, "file size 4");
        char buf[16] = {0};
        uint32_t r;
        CHECK(rd_file(f, 0, 4, buf, sizeof buf, &r) == 4 && r == 0 && !memcmp(buf, "BBBB", 4), "read the whole file");
        memset(buf, 0, sizeof buf);
        CHECK(rd_file(f, 2, 8, buf, sizeof buf, &r) == 2 && !memcmp(buf, "BB", 2), "a read past the end is cut at the end");
        CHECK(rd_file(f, 9, 4, buf, sizeof buf, &r) == 0 && r == 0, "a read beyond the end gives 0 bytes");
        uint8_t w[24] = {0};
        g = call(f, 1, w, sizeof w);
        CHECK(g.result == R_FS_INVALID_ARGUMENT, "Write is refused");
        g = call(f, 3, w, 8);
        CHECK(g.result == R_FS_INVALID_ARGUMENT, "SetSize is refused");
        uint8_t info[0x40];
        memset(info, 0xaa, sizeof info);
        CHECK(operate(f, 3, 0, 4, info) == 0 && !memcmp(info, (uint8_t[0x40]){0}, 0x40), "IFile::OperateRange QueryRange: an all-zero QueryRangeInfo");
        CHECK(operate(f, 2, 0, 4, NULL) == 0, "IFile::OperateRange InvalidateCache succeeds");
        CHECK(operate(f, 0, 0, 4, NULL) == R_FS_INVALID_ARGUMENT, "IFile::OperateRange Clear on a read-only file is refused");
        CHECK(operate(f, 99, 0, 4, NULL) == R_FS_INVALID_ARGUMENT, "an unknown operation is refused");
        g = call(f, 6, (uint8_t[24]){3}, 24);
        CHECK(g.result == 0, "OperateRangeWithBuffer answers like OperateRange");
    }
    mode = 6;
    c = call_path(fs, 8, "/a.txt", &mode, 4);
    CHECK(c.result == R_FS_INVALID_ARGUMENT && c.nout_objects == 0, "opening for writing is refused");
    mode = 1;
    c = call_path(fs, 8, "/dir", &mode, 4);
    CHECK(c.result == R_FS_PATH_NOT_FOUND, "a directory is not a file");

    mode = 3; /* directories and files */
    c = call_path(fs, 9, "/", &mode, 4);
    CHECK(c.result == 0 && c.nout_objects == 1, "OpenDirectory");
    if (c.nout_objects == 1) {
        IpcObject *d = c.out_objects[0];
        IpcCall g = call(d, 1, NULL, 0);
        CHECK(g.result == 0 && SVC_OUT(&g, int64_t, 0) == 2, "two entries in the root");
        static uint8_t ents[0x310 * 4];
        IpcCall r;
        memset(&r, 0, sizeof r);
        svc_set_out_buf(&r, ents, sizeof ents);
        svc_call_c(d, 0, &r);
        CHECK(r.result == 0 && SVC_OUT(&r, int64_t, 0) == 2, "Read returns both entries");
        CHECK(!strcmp((char *)ents, "dir") && ents[0x304] == 0, "first entry: dir");
        CHECK(!strcmp((char *)ents + 0x310, "a.txt") && ents[0x310 + 0x304] == 1 && *(int64_t *)(ents + 0x310 + 0x308) == 5, "second entry: a.txt, 5 bytes");
        memset(&r, 0, sizeof r);
        svc_set_out_buf(&r, ents, sizeof ents);
        svc_call_c(d, 0, &r);
        CHECK(r.result == 0 && SVC_OUT(&r, int64_t, 0) == 0, "a second Read is at the end");
    }
    mode = 2; /* files only */
    c = call_path(fs, 9, "/dir", &mode, 4);
    if (c.nout_objects == 1) {
        IpcCall g = call(c.out_objects[0], 1, NULL, 0);
        CHECK(g.result == 0 && SVC_OUT(&g, int64_t, 0) == 1, "files only: b.bin");
    }
    mode = 3;
    c = call_path(fs, 9, "/a.txt", &mode, 4);
    CHECK(c.result == R_FS_PATH_NOT_FOUND, "a file is not a directory");

    CHECK(call_path(fs, 0, "/new", NULL, 0).result == R_FS_INVALID_ARGUMENT, "CreateFile is refused");
    CHECK(call_path(fs, 2, "/new", NULL, 0).result == R_FS_INVALID_ARGUMENT, "CreateDirectory is refused");
    CHECK(call_path(fs, 1, "/a.txt", NULL, 0).result == R_FS_INVALID_ARGUMENT, "DeleteFile is refused");
    c = call(fs, 12, NULL, 0);
    CHECK(c.result == 0 && SVC_OUT(&c, int64_t, 0) > 0x140, "total space is the image size");
    c = call(fs, 11, NULL, 0);
    CHECK(c.result == 0 && SVC_OUT(&c, int64_t, 0) == 0, "free space 0");
    CHECK(call(fs, 10, NULL, 0).result == 0, "Commit succeeds");
}

static void t_program_id_rows(void) {
    uint8_t in[16] = {0};
    uint64_t id = APP;
    memcpy(in, &id, 8);
    IpcCall c = call(g_srv, 9, in, 8);
    CHECK(c.result == 0 && c.nout_objects == 1, "9 OpenDataFileSystemByProgramId for the running application");
    if (c.nout_objects == 1) {
        uint32_t t;
        CHECK(entry_type(c.out_objects[0], "/dir/sub/c.txt", &t) == 0 && t == 1, "...is the application's RomFS");
    }
    id = APP | 0x800; /* its update title */
    memcpy(in, &id, 8);
    c = call(g_srv, 9, in, 8);
    CHECK(c.result == 0 && c.nout_objects == 1, "the update title id also opens the (patched) RomFS");
    id = 0x0100000000099999ULL;
    memcpy(in, &id, 8);
    c = call(g_srv, 9, in, 8);
    CHECK(c.result == R_FS_NOT_FOUND_DATA && c.nout_objects == 0, "an unknown program id: not found");
    CHECK(call(g_srv, 9, in, 4).result == R_FS_INVALID_ARGUMENT, "a short request is rejected");

    id = APP;
    memcpy(in, &id, 8);
    c = call(g_srv, 201, in, 8);
    CHECK(c.result == 0 && c.nout_objects == 1, "201 OpenDataStorageByProgramId");
    if (c.nout_objects == 1) {
        IpcObject *st = c.out_objects[0];
        IpcCall g = call(st, 4, NULL, 0);
        CHECK(g.result == 0 && SVC_OUT(&g, int64_t, 0) == 0x140 + 24, "the storage is the RomFS image");
        uint8_t info[0x40];
        memset(info, 0xaa, sizeof info);
        CHECK(operate(st, 3, 0, 8, info) == 0 && !memcmp(info, (uint8_t[0x40]){0}, 0x40), "IStorage::OperateRange QueryRange: all-zero info");
        CHECK(operate(st, 2, 0, 8, NULL) == 0, "IStorage::OperateRange InvalidateCache");
        CHECK(operate(st, 0, 0, 8, NULL) == R_FS_INVALID_ARGUMENT, "IStorage::OperateRange Clear is refused (read-only)");
    }
    id = 0x0100000000099999ULL;
    memcpy(in, &id, 8);
    c = call(g_srv, 201, in, 8);
    CHECK(c.result == R_FS_NOT_FOUND_DATA && c.nout_objects == 0, "201 with an unknown id: not found");

    uint8_t index = 0;
    c = call(g_srv, 205, &index, 1);
    CHECK(c.result == 0 && c.nout_objects == 1, "205 OpenDataStorageWithProgramIndex 0 is the running program");
    index = 1;
    c = call(g_srv, 205, &index, 1);
    CHECK(c.result == R_FS_NOT_FOUND_DATA && c.nout_objects == 0, "another program index: not found");
}

static void t_content_file_systems(void) {
    uint8_t in[16] = {0};
    uint64_t id = APP;
    memcpy(in + 8, &id, 8);
    uint32_t type = 1; /* Rom */
    memcpy(in + 4, &type, 4);
    IpcCall c = call_path(g_srv, 10, "@Rom:/dummy.nca", in, 16);
    CHECK(c.result == 0 && c.nout_objects == 1, "10 OpenFileSystemWithId Rom (result %#x)", c.result);
    uint32_t t;
    if (c.nout_objects == 1) CHECK(entry_type(c.out_objects[0], "/a.txt", &t) == 0, "...serves the RomFS");
    type = 6; /* Data */
    memcpy(in + 4, &type, 4);
    c = call_path(g_srv, 10, "x", in, 16);
    CHECK(c.result == 0 && c.nout_objects == 1, "Data type serves the RomFS too");
    type = 3; /* Control */
    memcpy(in + 4, &type, 4);
    c = call_path(g_srv, 10, "x", in, 16);
    CHECK(c.result == 0 && c.nout_objects == 1, "Control opens (result %#x)", c.result);
    if (c.nout_objects == 1) {
        IpcObject *fs = c.out_objects[0];
        CHECK(entry_type(fs, "/control.nacp", &t) == 0 && t == 1, "control.nacp is there");
        CHECK(entry_type(fs, "/main.npdm", &t) == (int)R_FS_PATH_NOT_FOUND, "the other files of the data directory are not");
        uint32_t mode = 1;
        IpcCall o = call_path(fs, 8, "/control.nacp", &mode, 4);
        CHECK(o.result == 0 && o.nout_objects == 1, "open control.nacp");
        if (o.nout_objects == 1) {
            char buf[16] = {0};
            uint32_t r;
            CHECK(rd_file(o.out_objects[0], 0, 8, buf, sizeof buf, &r) == 8 && !memcmp(buf, "NACPDATA", 8), "its bytes");
        }
        CHECK(call_path(fs, 1, "/control.nacp", NULL, 0).result == R_FS_INVALID_ARGUMENT, "the content file system is read-only");
    }
    type = 4; /* Manual */
    memcpy(in + 4, &type, 4);
    c = call_path(g_srv, 10, "x", in, 16);
    CHECK(c.result == 0 && c.nout_objects == 1, "Manual opens the title's html directory");
    if (c.nout_objects == 1) CHECK(entry_type(c.out_objects[0], "/index.html", &t) == 0 && t == 1, "index.html is there");
    for (type = 0; type < 8; type++) {
        if (type == 1 || type == 3 || type == 4 || type == 6) continue;
        memcpy(in + 4, &type, 4);
        c = call_path(g_srv, 10, "x", in, 16);
        CHECK(c.result == R_FS_PATH_NOT_FOUND && c.nout_objects == 0, "type %u has no NCA on the host: path not found", type);
    }
    CHECK(call_path(g_srv, 10, "x", in, 8).result == R_FS_INVALID_ARGUMENT, "a short request is rejected");

    /* 7 OpenFileSystemWithPatch(u32 type, u64 program id): Manual only */
    uint8_t pin[16] = {0};
    type = 4;
    memcpy(pin, &type, 4);
    memcpy(pin + 8, &id, 8);
    c = call(g_srv, 7, pin, sizeof pin);
    CHECK(c.result == 0 && c.nout_objects == 1, "7 OpenFileSystemWithPatch Manual");
    type = 1;
    memcpy(pin, &type, 4);
    c = call(g_srv, 7, pin, sizeof pin);
    CHECK(c.result == R_FS_INVALID_ARGUMENT && c.nout_objects == 0, "7 with another type is invalid");
}

static void t_sd_card_and_log_state(void) {
    IpcCall c = call(g_srv, 27, NULL, 0);
    CHECK(c.result == 0 && SVC_OUT(&c, uint8_t, 0) == 1, "27 IsExFatSupported");
    c = call(g_srv, 631, NULL, 0);
    CHECK(c.result == 0 && SVC_OUT(&c, uint8_t, 0) == 1, "631 IsSdCardAccessible: accessible by default");
    uint8_t off = 0, on = 1;
    CHECK(call(g_srv, 630, &off, 1).result == 0, "630 SetSdCardAccessibility(false)");
    c = call(g_srv, 631, NULL, 0);
    CHECK(SVC_OUT(&c, uint8_t, 0) == 0, "...631 now says not accessible");
    call(g_srv, 630, &on, 1);
    c = call(g_srv, 631, NULL, 0);
    CHECK(SVC_OUT(&c, uint8_t, 0) == 1, "...and accessible again");
    uint8_t seed[16];
    for (int i = 0; i < 16; i++) seed[i] = (uint8_t)(0x10 + i);
    CHECK(call(g_srv, 620, seed, 16).result == 0 && !memcmp(g_sd_seed, seed, 16), "620 SetSdCardEncryptionSeed keeps the seed");
    CHECK(call(g_srv, 620, seed, 4).result == R_FS_INVALID_ARGUMENT, "620 with a short seed is invalid");

    c = call(g_srv, 1005, NULL, 0);
    CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "the global access log mode is 0 at first");
    uint32_t mode = 3;
    CHECK(call(g_srv, 1004, &mode, 4).result == 0, "1004 SetGlobalAccessLogMode");
    c = call(g_srv, 1005, NULL, 0);
    CHECK(SVC_OUT(&c, uint32_t, 0) == 3, "...1005 reads it back");
    c = call(g_srv, 1011, NULL, 0);
    CHECK(c.result == 0 && c.out_size == 8 && SVC_OUT(&c, uint32_t, 0) == 1 && SVC_OUT(&c, uint32_t, 4) == 0, "1011 GetProgramIndexForAccessLog: version 1, program 0");
    CHECK(call(g_srv, 1016, NULL, 0).result == 0, "1016 FlushAccessLogOnSdCard");
    c = call(g_srv, 612, (uint8_t[8]){1}, 8);
    CHECK(c.result == 0 && SVC_OUT(&c, int64_t, 0) > 0, "612 GetFreeSpaceSizeForSaveData: the volume has room");

    c = call(g_srv, 100, (uint32_t[1]){0}, 4);
    CHECK(c.result == 0 && c.nout_objects == 1, "100 OpenImageDirectoryFileSystem(NAND)");
    if (c.nout_objects == 1) {
        CHECK(call_path(c.out_objects[0], 2, "/Album", NULL, 0).result == 0, "...is a writable directory file system");
        uint32_t t;
        CHECK(entry_type(c.out_objects[0], "/Album", &t) == 0 && t == 0, "...the directory is there");
    }
    CHECK(call(g_srv, 100, (uint32_t[1]){1}, 4).result == 0, "100 for the SD card");
    CHECK(call(g_srv, 100, (uint32_t[1]){2}, 4).result == R_FS_INVALID_ENUM, "100 with an unknown id: 0x2EE202");
    c = call(g_srv, 130, (uint32_t[1]){0}, 4);
    CHECK(c.result == 0 && c.nout_objects == 1, "130 OpenCustomStorageFileSystem(user)");
    CHECK(call(g_srv, 130, (uint32_t[1]){1}, 4).result == 0, "130 for the SD card");
    CHECK(call(g_srv, 130, (uint32_t[1]){5}, 4).result == R_FS_INVALID_ENUM, "130 with an unknown id: 0x2EE202");
}

static void t_system_data_notifier(void) {
    IpcCall c = call(g_srv, 510, NULL, 0);
    CHECK(c.result == 0 && c.nout_objects == 1, "510 OpenSystemDataUpdateEventNotifier");
    if (c.nout_objects != 1) return;
    IpcCall e = call(c.out_objects[0], 0, NULL, 0);
    CHECK(e.result == 0 && e.nout_copy == 1, "GetEventHandle hands out an event");
    if (e.nout_copy != 1) return;
    CHECK(!event_is_signaled(e.out_copy[0]), "the event is not signalled at first");
    CHECK(call(g_srv, 511, NULL, 0).result == 0, "511 NotifySystemDataUpdateEvent");
    CHECK(event_is_signaled(e.out_copy[0]), "...and now it is");
}

/* Saves: create two, find them with a filter, open their internal storage and watch the commit id move. */
static void t_saves(void) {
    uint8_t ci[0x80] = {0};
    uint8_t *attr = ci;
    uint8_t uid[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    uint64_t prog = APP;
    memcpy(attr, &prog, 8);
    memcpy(attr + 8, uid, 16);
    attr[0x20] = SAVE_ACCOUNT;
    int64_t size = 0x400000, journal = 0x100000;
    memcpy(ci + 0x40, &size, 8);
    memcpy(ci + 0x48, &journal, 8);
    uint64_t own_a = 0x0100000000ABCD01ull, own_b = 0x0100000000ABCD02ull;
    memcpy(ci + 0x40 + 0x18, &own_a, 8);
    ci[0x40 + 0x24] = 1; /* user space */
    IpcCall c = call(g_srv, 22, ci, sizeof ci);
    CHECK(c.result == 0, "create an account save (result %#x)", c.result);
    memset(ci, 0, sizeof ci);
    memcpy(attr, &prog, 8);
    attr[0x20] = SAVE_DEVICE;
    memcpy(ci + 0x40, &size, 8);
    memcpy(ci + 0x40 + 0x18, &own_b, 8);
    ci[0x40 + 0x24] = 1;
    c = call(g_srv, 22, ci, sizeof ci);
    CHECK(c.result == 0, "create a device save");

    /* 67 / 68 with {u8 space, filter} */
    uint8_t fin[0x50] = {0};
    fin[0] = 1;                 /* user space */
    fin[8 + 1] = 1;             /* FilterSaveDataType */
    fin[8 + 8 + 0x20] = SAVE_ACCOUNT;
    uint8_t info[0x60 * 4] = {0};
    IpcCall f;
    memset(&f, 0, sizeof f);
    f.in = fin;
    f.in_size = sizeof fin;
    svc_set_out_buf(&f, info, sizeof info);
    svc_call_c(g_srv, 67, &f);
    CHECK(f.result == 0 && SVC_OUT(&f, int64_t, 0) == 1, "67 FindSaveDataWithFilter finds the account save (result %#x)", f.result);
    CHECK(info[8] == 1 && info[9] == SAVE_ACCOUNT && !memcmp(info + 0x10, uid, 16) && *(uint64_t *)(info + 0x28) == APP, "...with its SaveDataInfo");
    uint64_t account_id = *(uint64_t *)info;
    fin[8 + 8 + 0x20] = SAVE_CACHE;
    memset(&f, 0, sizeof f);
    f.in = fin;
    f.in_size = sizeof fin;
    svc_set_out_buf(&f, info, sizeof info);
    svc_call_c(g_srv, 67, &f);
    CHECK(f.result == 0 && SVC_OUT(&f, int64_t, 0) == 0, "no cache save: count 0");
    fin[8 + 8 + 0x20] = SAVE_ACCOUNT;
    fin[8 + 2] = 1; /* FilterSaveDataUserId with a different user */
    fin[8 + 8 + 8] = 0x77;
    memset(&f, 0, sizeof f);
    f.in = fin;
    f.in_size = sizeof fin;
    svc_set_out_buf(&f, info, sizeof info);
    svc_call_c(g_srv, 67, &f);
    CHECK(SVC_OUT(&f, int64_t, 0) == 0, "a user filter that does not match: count 0");
    CHECK(call(g_srv, 67, fin, 8).result == R_FS_INVALID_ARGUMENT, "67 with a short request is invalid");

    /* 84 ListAccessibleSaveDataOwnerId {s32 start, s32 reserved, u64 program id} -> s32 count + u64[] */
    {
        uint8_t in84[16] = {0};
        uint64_t ids[4] = {0};
        memcpy(in84 + 8, &prog, 8);
        IpcCall l = {0};
        l.in = in84; l.in_size = sizeof in84;
        svc_set_out_buf(&l, ids, sizeof ids);
        svc_call_c(g_srv, 84, &l);
        CHECK(l.result == 0 && SVC_OUT(&l, int32_t, 0) == 2 && ids[0] != ids[1] && (ids[0] == own_a || ids[1] == own_a) && (ids[0] == own_b || ids[1] == own_b), "84 lists the owner ids of the program's saves");
        memset(ids, 0, sizeof ids); in84[0] = 1;
        l = (IpcCall){0}; l.in = in84; l.in_size = sizeof in84; svc_set_out_buf(&l, ids, sizeof ids);
        svc_call_c(g_srv, 84, &l);
        CHECK(SVC_OUT(&l, int32_t, 0) == 1 && ids[0] != 0 && ids[1] == 0, "84 honours the start index");
        in84[0] = 0; uint64_t other = APP + 1; memcpy(in84 + 8, &other, 8);
        l = (IpcCall){0}; l.in = in84; l.in_size = sizeof in84; svc_set_out_buf(&l, ids, sizeof ids);
        svc_call_c(g_srv, 84, &l);
        CHECK(l.result == 0 && SVC_OUT(&l, int32_t, 0) == 0, "84 for a program without saves: count 0");
        memcpy(in84 + 8, &prog, 8);
        l = (IpcCall){0}; l.in = in84; l.in_size = sizeof in84; svc_set_out_buf(&l, ids, 8);
        svc_call_c(g_srv, 84, &l);
        CHECK(SVC_OUT(&l, int32_t, 0) == 1, "84 is cut to the buffer");
        CHECK(call(g_srv, 84, in84, 8).result == R_FS_INVALID_ARGUMENT, "84 with a short request is invalid");
    }

    uint8_t fin2[0x50] = {0};
    fin2[0] = 1;
    c = call(g_srv, 68, fin2, sizeof fin2); /* no flags set: both saves */
    CHECK(c.result == 0 && c.nout_objects == 1, "68 OpenSaveDataInfoReaderWithFilter");
    if (c.nout_objects == 1) {
        IpcCall r;
        memset(&r, 0, sizeof r);
        svc_set_out_buf(&r, info, sizeof info);
        svc_call_c(c.out_objects[0], 0, &r);
        CHECK(r.result == 0 && SVC_OUT(&r, int64_t, 0) == 2, "...reads both saves (got %lld)", (long long)SVC_OUT(&r, int64_t, 0));
    }
    fin2[8 + 1] = 1;
    fin2[8 + 8 + 0x20] = SAVE_DEVICE;
    c = call(g_srv, 68, fin2, sizeof fin2);
    if (c.nout_objects == 1) {
        IpcCall r;
        memset(&r, 0, sizeof r);
        svc_set_out_buf(&r, info, sizeof info);
        svc_call_c(c.out_objects[0], 0, &r);
        CHECK(SVC_OUT(&r, int64_t, 0) == 1 && info[9] == SAVE_DEVICE, "...a type filter keeps only the device save");
    }

    /* 64 OpenSaveDataInternalStorageFileSystem + 616 GetSaveDataCommitId */
    uint8_t sid[16] = {1};
    memcpy(sid + 8, &account_id, 8);
    c = call(g_srv, 64, sid, sizeof sid);
    CHECK(c.result == 0 && c.nout_objects == 1, "64 OpenSaveDataInternalStorageFileSystem (result %#x)", c.result);
    IpcObject *internal = c.nout_objects == 1 ? c.out_objects[0] : NULL;
    memset(sid + 8, 0x55, 8);
    CHECK(call(g_srv, 64, sid, sizeof sid).result == R_FS_TARGET_NOT_FOUND, "64 with an unknown id: target not found");
    memcpy(sid + 8, &account_id, 8);
    c = call(g_srv, 616, sid, sizeof sid);
    CHECK(c.result == 0, "616 GetSaveDataCommitId");
    uint64_t commit0 = c.result ? 0 : SVC_OUT(&c, uint64_t, 0);
    if (internal) {
        CHECK(call_path(internal, 0, "/slot0", (uint8_t[16]){0}, 16).result == 0, "a file can be created through the internal storage");
        CHECK(call(internal, 10, NULL, 0).result == 0, "Commit");
        c = call(g_srv, 616, sid, sizeof sid);
        uint64_t commit1 = SVC_OUT(&c, uint64_t, 0);
        CHECK(commit1 > commit0, "the commit id advanced (%llx -> %llx)", (unsigned long long)commit0, (unsigned long long)commit1);
        call(internal, 10, NULL, 0);
        c = call(g_srv, 616, sid, sizeof sid);
        CHECK(SVC_OUT(&c, uint64_t, 0) > commit1, "...and advances on every commit");
    }
    memset(sid + 8, 0x66, 8);
    CHECK(call(g_srv, 616, sid, sizeof sid).result == R_FS_TARGET_NOT_FOUND, "616 with an unknown id: target not found");
}

static void t_host_file_operate_range(void) {
    IpcCall c = call(g_srv, 18, NULL, 0); /* the SD card file system */
    CHECK(c.result == 0 && c.nout_objects == 1, "SD card file system");
    if (c.nout_objects != 1) return;
    IpcObject *fs = c.out_objects[0];
    CHECK(call_path(fs, 0, "/f.bin", (uint8_t[16]){0, 0, 0, 0, 0, 0, 0, 0, 16}, 16).result == 0, "create a 16 byte file");
    uint32_t mode = 3;
    c = call_path(fs, 8, "/f.bin", &mode, 4);
    CHECK(c.result == 0 && c.nout_objects == 1, "open for read and write");
    if (c.nout_objects != 1) return;
    IpcObject *f = c.out_objects[0];
    uint8_t data[16];
    memset(data, 0xab, sizeof data);
    uint8_t win[24] = {0};
    int64_t sz = 16;
    memcpy(win + 16, &sz, 8);
    IpcCall w;
    memset(&w, 0, sizeof w);
    w.in = win;
    w.in_size = sizeof win;
    svc_set_in_buf(&w, data, sizeof data);
    svc_call_c(f, 1, &w);
    CHECK(w.result == 0, "write 16 bytes");
    uint8_t info[0x40];
    memset(info, 0x11, sizeof info);
    CHECK(operate(f, 3, 0, 16, info) == 0 && !memcmp(info, (uint8_t[0x40]){0}, 0x40), "QueryRange on a host file: an all-zero QueryRangeInfo");
    CHECK(operate(f, 0, 4, 8, NULL) == 0, "Clear zero-fills [4, 12)");
    uint8_t back[16];
    uint32_t r;
    CHECK(rd_file(f, 0, 16, back, sizeof back, &r) == 16, "read back");
    CHECK(back[3] == 0xab && back[4] == 0 && back[11] == 0 && back[12] == 0xab, "...the range is zero, its surroundings are untouched");
    CHECK(operate(f, 1, 0, 16, NULL) == 0, "ClearSignature succeeds");
    CHECK(operate(f, 3, -1, 16, NULL) == R_FS_INVALID_ARGUMENT, "a negative offset is refused");
}

/* romfs_read.c on its own: lookups, listing, reads, and images that are not (or not quite) a RomFS. */
typedef struct {
    const uint8_t *p;
    size_t n;
} Mem;
static ssize_t mem_read(void *ctx, void *dst, size_t len, uint64_t off) {
    const Mem *m = ctx;
    if (off >= m->n) return -1;
    if (len > m->n - off) len = m->n - (size_t)off;
    memcpy(dst, m->p + off, len);
    return (ssize_t)len;
}
static void t_romfs_reader(void) {
    uint8_t img[0x400];
    Mem m = {img, romfs_fixture_build(img)};
    RomfsImg *r = romfs_img_open(mem_read, &m, m.n);
    CHECK(r != NULL, "the fixture image parses");
    if (!r) return;
    uint64_t off, size;
    CHECK(romfs_img_lookup(r, "/a.txt", &off, &size) == ROMFS_FILE && off == 0 && size == 5, "a.txt: offset 0, 5 bytes");
    CHECK(romfs_img_lookup(r, "dir/b.bin", &off, &size) == ROMFS_FILE && off == 8 && size == 4, "a path without the leading slash");
    CHECK(romfs_img_lookup(r, "/dir/sub/", NULL, NULL) == ROMFS_DIR && romfs_img_lookup(r, "", NULL, NULL) == ROMFS_DIR && romfs_img_lookup(r, "/", NULL, NULL) == ROMFS_DIR, "directories, with a trailing slash and the root");
    CHECK(romfs_img_lookup(r, "/dir/a.txt", NULL, NULL) == ROMFS_NONE && romfs_img_lookup(r, "/a.txt/x", NULL, NULL) == ROMFS_NONE && romfs_img_lookup(r, "/DIR", NULL, NULL) == ROMFS_NONE, "wrong parent, file used as a directory, case matters");
    char name[32];
    int isdir;
    uint64_t sz;
    CHECK(romfs_img_child(r, "/", 0, name, sizeof name, &isdir, &sz) && !strcmp(name, "dir") && isdir, "root child 0: the directory first");
    CHECK(romfs_img_child(r, "/", 1, name, sizeof name, &isdir, &sz) && !strcmp(name, "a.txt") && !isdir && sz == 5, "root child 1: the file");
    CHECK(!romfs_img_child(r, "/", 2, name, sizeof name, &isdir, &sz) && !romfs_img_child(r, "/a.txt", 0, name, sizeof name, &isdir, &sz), "past the end, and a file has no children");
    char buf[8] = {0};
    romfs_img_lookup(r, "/dir/sub/c.txt", &off, &size);
    CHECK(romfs_img_read(r, off, size, 0, buf, sizeof buf) == 3 && !memcmp(buf, "CCC", 3), "read c.txt (cut at its size)");
    CHECK(romfs_img_read(r, off, size, 1, buf, 1) == 1 && buf[0] == 'C' && romfs_img_read(r, off, size, 3, buf, 1) == 0, "read from the middle; at the end 0");
    CHECK(romfs_img_read(r, 1u << 30, 4, 0, buf, 4) == -1, "a file extent beyond the image is an error");
    romfs_img_close(r);

    Mem tiny = {img, 0x20};
    CHECK(romfs_img_open(mem_read, &tiny, 0x20) == NULL, "an image shorter than the header is refused");
    uint8_t bad[0x400];
    memcpy(bad, img, sizeof bad);
    uint64_t huge = 1ull << 40;
    memcpy(bad + 0x20, &huge, 8); /* directory table size */
    Mem bm = {bad, m.n};
    CHECK(romfs_img_open(mem_read, &bm, m.n) == NULL, "a directory table that does not fit is refused");
    memcpy(bad, img, sizeof bad);
    uint64_t wrong = 0x40;
    memcpy(bad, &wrong, 8); /* header size */
    CHECK(romfs_img_open(mem_read, &bm, m.n) == NULL, "a wrong header size is refused");
    memcpy(bad, img, sizeof bad);
    uint32_t loop = 0; /* a file whose sibling is itself: the walk must end */
    memcpy(bad + 0xc0 + 4, &loop, 4);
    r = romfs_img_open(mem_read, &bm, m.n);
    CHECK(r && romfs_img_lookup(r, "/nope.txt", NULL, NULL) == ROMFS_NONE, "a sibling loop in the file table does not hang the lookup");
    for (unsigned i = 0; r && i < 5; i++) romfs_img_child(r, "/", i, name, sizeof name, &isdir, &sz);
    romfs_img_close(r);
    memcpy(bad, img, sizeof bad);
    uint32_t far = 0x7fffff00;
    memcpy(bad + 0x60 + 8, &far, 4); /* the root's child directory points far outside the table */
    r = romfs_img_open(mem_read, &bm, m.n);
    CHECK(r && romfs_img_lookup(r, "/dir", NULL, NULL) == ROMFS_NONE, "a directory offset outside the table ends the walk (the directory is not found)");
    CHECK(r && romfs_img_child(r, "/", 0, name, sizeof name, &isdir, &sz) && !strcmp(name, "a.txt") && !isdir && !romfs_img_child(r, "/", 1, name, sizeof name, &isdir, &sz), "...and listing the root skips it");
    romfs_img_close(r);
}

int main(void) {
    char dir[256];
    snprintf(dir, sizeof dir, "%s/svc_fs_local.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(dir)) {
        printf("mkdtemp failed\n");
        return 1;
    }
    static char data[300], saves[300];
    snprintf(data, sizeof data, "%s/data", dir);
    snprintf(saves, sizeof saves, "%s/saves", dir);
    mkdir(data, 0755);
    mkdir(saves, 0755);
    char p[400];
    uint8_t img[0x400];
    size_t n = romfs_fixture_build(img);
    snprintf(p, sizeof p, "%s/romfs.bin", data);
    put(p, img, n);
    snprintf(p, sizeof p, "%s/control.nacp", data);
    put(p, "NACPDATA", 8);
    snprintf(p, sizeof p, "%s/main.npdm", data);
    put(p, "NPDM", 4);
    snprintf(p, sizeof p, "%s/html", data);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/html/index.html", data);
    put(p, "<html>", 6);
    g_cfg.data_dir = data;
    g_cfg.save_root = saves;
    snprintf(g_cfg.title_id, sizeof g_cfg.title_id, "0100000000010000");
    g_program_id = APP;

    g_srv = fsp_open();
    t_romfs_reader();
    t_romfs_file_system();
    t_program_id_rows();
    t_content_file_systems();
    t_sd_card_and_log_state();
    t_system_data_notifier();
    t_saves();
    t_host_file_operate_range();

    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) printf("cleanup failed\n");
    printf("{\"test\":\"svc_fs_local\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
