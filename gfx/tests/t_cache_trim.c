/* t_cache_trim.c - the shader cache size cap: oldest files go first, the prewarm manifests stay, dry runs remove nothing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "cache_trim.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } } while (0)

static void put(const char *path, size_t n, long age_s) {
    FILE *f = fopen(path, "wb");
    if (!f) { printf("cannot create %s\n", path); exit(2); }
    for (size_t i = 0; i < n; i++) fputc('x', f);
    fclose(f);
    struct timeval tv[2];
    gettimeofday(&tv[0], NULL);
    tv[0].tv_sec -= age_s; tv[1] = tv[0];
    utimes(path, tv);
}
static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

int main(void) {
    char root[] = "build/t_cache_trim-XXXXXX";
    mkdir("build", 0755);
    if (!mkdtemp(root)) { perror("mkdtemp"); return 2; }
    char p[4][256];
    char sub[256]; snprintf(sub, sizeof sub, "%s/shaders", root); mkdir(sub, 0755);
    snprintf(p[0], 256, "%s/shaders/old.msl", root);   put(p[0], 1000, 3000);
    snprintf(p[1], 256, "%s/shaders/mid.msl", root);   put(p[1], 1000, 2000);
    snprintf(p[2], 256, "%s/pipelines-v9.metalar", root); put(p[2], 1000, 100);
    snprintf(p[3], 256, "%s/pipelines.bin", root);     put(p[3], 500, 9000);

    uint64_t before = 0;
    CHECK(gfx_cache_trim(root, 0, 0, &before) == 0 && before == 3500, "unlimited cap removes nothing and reports the total");
    CHECK(gfx_cache_trim(root, 2600, 1, NULL) == 1000 && exists(p[0]), "dry run reports but keeps the file");
    CHECK(gfx_cache_trim(root, 2600, 0, NULL) == 1000, "trims exactly the oldest file to get under the cap");
    CHECK(!exists(p[0]) && exists(p[1]) && exists(p[2]) && exists(p[3]), "oldest removed, newer and manifest kept");
    CHECK(gfx_cache_trim(root, 1, 0, NULL) == 2000, "a tiny cap removes every evictable file");
    CHECK(!exists(p[1]) && !exists(p[2]) && exists(p[3]), "the manifest is never removed");
    CHECK(gfx_cache_trim("build/does-not-exist", 10, 0, &before) == 0 && before == 0, "missing directory is fine");
    setenv("NAPPLE_GFX_CACHE_MAX_MB", "3", 1);
    CHECK(gfx_cache_max_bytes() == (3u << 20), "env override in MB");
    unsetenv("NAPPLE_GFX_CACHE_MAX_MB");
    CHECK(gfx_cache_max_bytes() == ((uint64_t)GFX_CACHE_DEFAULT_MAX_MB << 20), "default cap");

    unlink(p[3]); rmdir(sub); rmdir(root);
    printf(fails ? "FAILED\n" : "t_cache_trim: all passed\n");
    return fails != 0;
}
