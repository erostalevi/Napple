/* Unit test: fsp-srv command 30 OpenDataStorageByDataId. fs.c is #included (static handlers); misc.o supplies the strong aoc_data_id_romfs, fed by a
 * manifest.json written to a temporary data dir. */
#include "../src/services/fs.c"
#include "svc_test.h"
#include <unistd.h>

#define APP 0x0100000000010000ULL

static void put(const char *path, const void *p, size_t n) { FILE *f = fopen(path, "wb"); if (f) { fwrite(p, 1, n, f); fclose(f); } }

int main(void) {
    char dir[256]; snprintf(dir, sizeof dir, "%s/svc_fs_dataid.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(dir)) { printf("mkdtemp failed\n"); return 1; }
    char path[320];
    snprintf(path, sizeof path, "%s/dlc", dir); mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/dlc/romfs.bin", dir); const char romfs[40] = "DLCROMFS"; put(path, romfs, sizeof romfs);
    const char *manifest = "{\"format\":1,\"title_id\":\"0100000000010000\",\"dlc\":[{\"title_id\":\"0100000000011001\",\"version\":0,\"dir\":\"dlc\",\"romfs\":\"romfs.bin\",\"default_enabled\":true},"
                           "{\"title_id\":\"0100000000011002\",\"version\":0,\"dir\":\"nodir\",\"romfs\":\"romfs.bin\",\"default_enabled\":true}]}";
    snprintf(path, sizeof path, "%s/manifest.json", dir); put(path, manifest, strlen(manifest));
    static char data_dir[256]; snprintf(data_dir, sizeof data_dir, "%s", dir); g_cfg.data_dir = data_dir; g_program_id = APP;

    IpcObject *srv = fsp_open(); IpcCall c; uint8_t in[16];
    uint64_t id = 0x0100000000011001ULL; memset(in, 0, sizeof in); memcpy(in + 8, &id, 8);
    svc_call(srv, 30, in, sizeof in, &c);
    CHECK(c.result == 0 && c.nout_objects == 1, "a DLC data id opens a storage (result %#x)", c.result);
    if (c.nout_objects == 1) {
        IpcObject *st = c.out_objects[0];
        svc_call(st, 4, NULL, 0, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint64_t, 0) == sizeof romfs, "GetSize is the DLC romfs size");
        uint8_t buf[8] = {0}; uint64_t rd[2] = {0, 8}; memset(&c, 0, sizeof c); c.in = (const uint8_t *)rd; c.in_size = 16; svc_set_out_buf(&c, buf, sizeof buf); svc_call_c(st, 0, &c);
        CHECK(c.result == 0 && !memcmp(buf, "DLCROMFS", 8), "Read returns the DLC romfs bytes");
    }
    id = 0x0100000000011002ULL; memcpy(in + 8, &id, 8); svc_call(srv, 30, in, sizeof in, &c);
    CHECK(c.result == R_FS_NOT_FOUND_DATA && c.nout_objects == 0, "a DLC whose romfs is missing fails with NOT_FOUND_DATA");
    id = 0x0100000000011009ULL; memcpy(in + 8, &id, 8); svc_call(srv, 30, in, sizeof in, &c);
    CHECK(c.result == R_FS_NOT_FOUND_DATA && c.nout_objects == 0, "an unknown data id fails with NOT_FOUND_DATA");
    id = APP; memcpy(in + 8, &id, 8); svc_call(srv, 30, in, sizeof in, &c);
    CHECK(c.result == R_FS_NOT_FOUND_DATA, "the application's own id is not a DLC");


    /* BKTR delta layer: <data>/romfs.delta over a sibling build dir's romfs.bin: [0,4) base "BASE", [4,8) patch "PTCH", [8,12) base+8 "base". */
    {
        char root[300]; snprintf(root, sizeof root, "%s/dl", dir); mkdir(root, 0755);
        char bd[340], ud[340], dd[400]; snprintf(bd, sizeof bd, "%s/b0", root); snprintf(ud, sizeof ud, "%s/u1", root); snprintf(dd, sizeof dd, "%s/romfs.delta", ud);
        mkdir(bd, 0755); mkdir(ud, 0755); mkdir(dd, 0755);
        snprintf(path, sizeof path, "%s/romfs.bin", bd); put(path, "BASEAAAAbaseZZZZ", 16);
        snprintf(path, sizeof path, "%s/patch.bin", dd); put(path, "xxPTCHyy", 8);
        snprintf(path, sizeof path, "%s/romfs.json", dd); put(path, "{\"base_dir\":\"b0\"}", 18);
        uint8_t rl[16 + 3 * 24]; memset(rl, 0, sizeof rl); memcpy(rl, "RLC1", 4); uint32_t n = 3; uint64_t ds = 12; memcpy(rl + 4, &n, 4); memcpy(rl + 8, &ds, 8);
        uint64_t e[3][2] = {{0, 0}, {4, 2}, {8, 8}}; uint32_t src[3] = {0, 1, 0};
        for (int i = 0; i < 3; i++) { memcpy(rl + 16 + i * 24, e[i], 16); memcpy(rl + 16 + i * 24 + 16, &src[i], 4); }
        snprintf(path, sizeof path, "%s/romfs.reloc", dd); put(path, rl, sizeof rl);
        static char ud_s[340]; snprintf(ud_s, sizeof ud_s, "%s", ud); g_cfg.data_dir = ud_s;
        IpcObject *srv2 = fsp_open(); memset(&c, 0, sizeof c); svc_call(srv2, 200, NULL, 0, &c);
        CHECK(c.result == 0 && c.nout_objects == 1, "delta: OpenDataStorageByCurrentProcess opens (result %#x)", c.result);
        if (c.nout_objects == 1) {
            IpcObject *st = c.out_objects[0];
            svc_call(st, 4, NULL, 0, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint64_t, 0) == 12, "delta: GetSize is the patched size");
            uint8_t buf[12] = {0}; uint64_t rd[2] = {0, 12}; memset(&c, 0, sizeof c); c.in = (const uint8_t *)rd; c.in_size = 16; svc_set_out_buf(&c, buf, sizeof buf); svc_call_c(st, 0, &c);
            CHECK(c.result == 0 && !memcmp(buf, "BASEPTCHbase", 12), "delta: a read across extents composes base and patch");
        }
        g_cfg.data_dir = data_dir;
        snprintf(path, sizeof path, "%s/romfs.bin", bd); unlink(path); snprintf(path, sizeof path, "%s/patch.bin", dd); unlink(path);
        snprintf(path, sizeof path, "%s/romfs.json", dd); unlink(path); snprintf(path, sizeof path, "%s/romfs.reloc", dd); unlink(path);
        rmdir(dd); rmdir(bd); rmdir(ud); rmdir(root);
    }
    snprintf(path, sizeof path, "%s/dlc/romfs.bin", dir); unlink(path); snprintf(path, sizeof path, "%s/dlc", dir); rmdir(path);
    snprintf(path, sizeof path, "%s/manifest.json", dir); unlink(path); rmdir(dir);
    printf("{\"test\":\"svc_fs_dataid\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
