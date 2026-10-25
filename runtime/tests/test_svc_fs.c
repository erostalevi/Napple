/* Unit tests: fsp-srv OpenDataStorageByDataId (202), served from <data-dir>/sysdata/<id>.bin. fs.c is #included. */
#include "../src/services/fs.c"
#include "svc_test.h"

int main(void) {
    char dir[64] = "/tmp/napple-fsd-XXXXXX"; CHECK(mkdtemp(dir) != NULL, "temp dir");
    g_cfg.data_dir = dir;
    char sub[200]; snprintf(sub, sizeof sub, "%s/sysdata", dir); mkdir(sub, 0755);
    char f[260]; snprintf(f, sizeof f, "%s/0100000000000811.bin", sub);
    FILE *fp = fopen(f, "wb"); fwrite("0123456789", 1, 10, fp); fclose(fp);

    IpcObject *srv = fsp_open();
    uint8_t in[16] = {0}; uint64_t id = 0x0100000000000811ULL; memcpy(in + 8, &id, 8);
    IpcCall c; svc_call(srv, 202, in, sizeof in, &c);
    CHECK(c.result == 0 && c.nout_objects == 1, "known data id opens an IStorage");
    if (c.nout_objects) {
        IpcObject *st = c.out_objects[0]; IpcCall g; svc_call(st, 4, NULL, 0, &g);
        CHECK(g.result == 0 && SVC_OUT(&g, int64_t, 0) == 10, "storage size is the archive size");
    }
    id = 0x0100000000000999ULL; memcpy(in + 8, &id, 8);
    svc_call(srv, 202, in, sizeof in, &c); CHECK(c.result == R_FS_NOT_FOUND_DATA && c.nout_objects == 0, "unknown data id: not found");
    svc_call(srv, 202, in, 4, &c); CHECK(c.result == R_FS_INVALID_ARGUMENT, "short request is rejected");
    /* title-level layout: <title>/sysdata/<id>.bin with data dir <title>/<build id> */
    char bd[200]; snprintf(bd, sizeof bd, "%s/build", dir); mkdir(bd, 0755); g_cfg.data_dir = bd;
    id = 0x0100000000000811ULL; memcpy(in + 8, &id, 8);
    svc_call(srv, 202, in, sizeof in, &c); CHECK(c.result == 0 && c.nout_objects == 1, "archive one level above the data dir is found");
    g_cfg.data_dir = dir; rmdir(bd);
    unlink(f); rmdir(sub); rmdir(dir);
    printf("%d checks, %d failed\n", g_checked, g_failed);
    return g_failed != 0;
}
