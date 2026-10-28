/* Fuzz target: the mod overlay of services/romfs_map.c (<data-dir>/mods/overlay.json, one entry per line): load, resolve paths and list directories. */
#include "../../src/services/romfs_map.h"
#include "fuzz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int fuzz_romfs_overlay(const uint8_t *d, size_t n) {
    char dir[600], path[700];
    snprintf(dir, sizeof dir, "%s/ovl", fuzz_workdir());
    snprintf(path, sizeof path, "%s/mods", dir);
    mkdir(dir, 0755);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/mods/overlay.json", dir);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(d, 1, n, f);
    fclose(f);
    RomfsOverlay *o = romfs_overlay_load(dir);
    if (o) {
        char host[256], name[64];
        uint64_t size;
        int is_dir;
        /* query with every quoted string of the file (that is where the paths are), plus the root and an unrelated path */
        const char *queries[10] = {"/", "/nope", "/a/b"};
        char *z = malloc(n + 1), *save = z;
        memcpy(z, d, n);
        z[n] = 0;
        unsigned nq = 3;
        for (char *p = z; nq < 10 && (p = strchr(p, '"')); p++) {
            char *q = strchr(p + 1, '"');
            if (!q) break;
            *q = 0;
            queries[nq++] = p + 1;
            p = q;
        }
        for (unsigned i = 0; i < nq; i++) {
            romfs_overlay_lookup(o, queries[i], host, sizeof host, &size);
            romfs_overlay_lookup(o, queries[i], host, 8, NULL);
            for (size_t k = 0; k < 6; k++) {
                if (!romfs_overlay_child(o, queries[i], k, name, sizeof name, &is_dir)) break;
                romfs_overlay_child(o, queries[i], k, name, 4, NULL);
            }
        }
        romfs_overlay_lookup(NULL, "/", host, sizeof host, &size);
        free(save);
        romfs_overlay_free(o);
    }
    return 0;
}

void fuzz_romfs_overlay_seeds(FuzzSeedFn add, void *ctx) {
    static const char doc[] = "{\"overlay\":[\n"
                              "{\"path\":\"/Sound/bgm/title.opus\",\"file\":\"mods/files/0/Sound/bgm/title.opus\",\"size\":123456},\n"
                              "{\"path\":\"/Sound/se/jump.wav\",\"file\":\"mods/files/0/Sound/se/jump.wav\",\"size\":2048},\n"
                              "{\"path\":\"/Gfx/logo.png\",\"file\":\"mods/files/1/Gfx/logo.png\",\"size\":99},\n"
                              "{\"delete\":\"/Movie\"},\n"
                              "{\"delete\":\"/Data/old.bin\"}\n"
                              "]}\n";
    add(ctx, (const uint8_t *)doc, sizeof doc - 1);
    static const char tiny[] = "{\"path\":\"/a\",\"file\":\"mods/a\",\"size\":1}\n";
    add(ctx, (const uint8_t *)tiny, sizeof tiny - 1);
}
FUZZ_TARGET(romfs_overlay);
