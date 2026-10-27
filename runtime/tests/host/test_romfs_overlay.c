/* romfs overlay lookup (mods/overlay.json written by napple build --mods). Portable: runs on any POSIX host. */
#include "../check.h"
#include "romfs_map.h"
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

static void put(const char *path, const char *body) {
    FILE *f = fopen(path, "wb");
    fputs(body, f);
    fclose(f);
}

int main(void) {
    char dir[] = "/tmp/napple-overlay-XXXXXX";
    if (!mkdtemp(dir)) return 2;
    char p[4096];
    snprintf(p, sizeof p, "%s/mods", dir);
    mkdir(p, 0755);
    CHECK(romfs_overlay_load(dir) == NULL, "absent overlay.json gives NULL");
    snprintf(p, sizeof p, "%s/mods/overlay.json", dir);
    put(p, "{\"format\":1,\"entries\":[\n"
           "{\"path\":\"/ui/title.txt\",\"file\":\"mods/files/0/ui/title.txt\",\"size\":13},\n"
           "{\"path\":\"/ui/deep/x.bin\",\"file\":\"mods/files/0/ui/deep/x.bin\",\"size\":4},\n"
           "{\"path\":\"/ui/b.bin\",\"file\":\"mods/files/1/ui/b.bin\",\"size\":9},\n"
           "{\"delete\":\"/audio/old.bin\"},\n"
           "{\"delete\":\"/gone\"}\n"
           "]}\n");
    RomfsOverlay *o = romfs_overlay_load(dir);
    CHECK(o != NULL, "loads");
    char host[4096];
    uint64_t sz = 0;
    CHECK(romfs_overlay_lookup(o, "/ui/title.txt", host, sizeof host, &sz) == ROMFS_OVERLAY_REPLACED, "replaced");
    char want[4096];
    snprintf(want, sizeof want, "%s/mods/files/0/ui/title.txt", dir);
    CHECK(!strcmp(host, want) && sz == 13, "host path and size: %s %llu", host, (unsigned long long)sz);
    CHECK(romfs_overlay_lookup(o, "/audio/old.bin", host, sizeof host, &sz) == ROMFS_OVERLAY_DELETED, "deleted file");
    CHECK(romfs_overlay_lookup(o, "/gone/sub/file", host, sizeof host, &sz) == ROMFS_OVERLAY_DELETED, "deleted subtree");
    CHECK(romfs_overlay_lookup(o, "/gone2", host, sizeof host, &sz) == ROMFS_OVERLAY_ORIGINAL, "prefix without boundary is untouched");
    CHECK(romfs_overlay_lookup(o, "/other.bin", host, sizeof host, &sz) == ROMFS_OVERLAY_ORIGINAL, "untouched file");
    CHECK(romfs_overlay_lookup(NULL, "/x", host, sizeof host, &sz) == ROMFS_OVERLAY_ORIGINAL, "no overlay = original");

    char name[64];
    int isdir = -1;
    CHECK(romfs_overlay_child(o, "/ui", 0, name, sizeof name, &isdir) && !strcmp(name, "title.txt") && !isdir, "child 0");
    CHECK(romfs_overlay_child(o, "/ui", 1, name, sizeof name, &isdir) && !strcmp(name, "deep") && isdir, "child 1 is a directory");
    CHECK(romfs_overlay_child(o, "/ui", 2, name, sizeof name, &isdir) && !strcmp(name, "b.bin"), "child 2");
    CHECK(!romfs_overlay_child(o, "/ui", 3, name, sizeof name, &isdir), "no child 3");
    CHECK(romfs_overlay_child(o, "/", 0, name, sizeof name, &isdir) && !strcmp(name, "ui") && isdir, "root child");
    romfs_overlay_free(o);

    put(p, "{\"entries\":[\n{\"path\":\"/a\",\"file\":\"../../etc/passwd\",\"size\":1}\n]}\n");
    CHECK(romfs_overlay_load(dir) == NULL, "escaping host file is refused");
    put(p, "{\"entries\":[\n{\"delete\":\"/a/../b\"}\n]}\n");
    CHECK(romfs_overlay_load(dir) == NULL, "dotdot path is refused");
    put(p, "{\"entries\":[\n]}\n");
    CHECK(romfs_overlay_load(dir) == NULL, "empty overlay is NULL");

    printf("romfs overlay: %d checks, %d failed\n", g_checked, g_failed);
    return g_failed != 0;
}
