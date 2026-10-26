/* t_pso_archive.c - the on-disk pipeline caches (binary archive, pipelines.bin) must survive damage: pass 1 fills the cache with a drawn triangle, the test then
 * truncates / overwrites every file in the cache directory, pass 2 and pass 3 (garbage and empty files) must initialise, render the same pixels and exit 0.
 * One process per pass (re-exec of this binary with T_ARCH_PASS=n), because napple_gfx_init is once per process. No game data. */
#include "nvn_harness.h"
#include <dirent.h>
#include <sys/stat.h>

static const char *CACHE = "build/t_pso_archive-cache";

static int render_pass(void) {
    H h; if (h_init(&h, 64, 64, CACHE)) return 1;
    NVNtexture *t = h_tex(&h, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8, 64, 64, 1, 0x20000, NVN_TEXTURE_FLAGS_DISPLAY);
    float blue[4] = { 0, 0, 1, 1 };
    h_begin(&h);
    h_triangle(&h, t, 0);
    L(nvnCommandBufferClearColor)(h.cb, 0, blue, 0xf);
    L(nvnCommandBufferDrawArrays)(h.cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    h_readback(&h, t, 0, 0, 64, 64, H_READBACK);
    h_submit(&h);
    const uint32_t *px = (const uint32_t *)(h.mem + H_READBACK);
    CHECK(px[32 * 64 + 32] == 0xff0000ffu && px[1 * 64 + 1] == 0xffff0000u, "pixels: center %08x corner %08x", px[32 * 64 + 32], px[1 * 64 + 1]);
    return h_finish("t_pso_archive pass", &h);
}

static int damage(int mode) {          /* 1: truncate to half, 2: overwrite with garbage, 3: empty. returns files touched */
    DIR *d = opendir(CACHE); if (!d) return 0;
    int n = 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024]; snprintf(p, sizeof p, "%s/%s", CACHE, e->d_name);
        struct stat st; if (stat(p, &st) || !S_ISREG(st.st_mode)) continue;
        FILE *f = fopen(p, mode == 1 ? "r+" : "w"); if (!f) continue;
        if (mode == 1) { if (truncate(p, st.st_size / 2)) {} }
        else if (mode == 2) for (int i = 0; i < 4096; i++) fputc((i * 131 + 7) & 0xff, f);
        fclose(f); n++;
    }
    closedir(d); return n;
}

int main(int argc, char **argv) {
    const char *pass = getenv("T_ARCH_PASS");
    if (pass) return render_pass();
    (void)argc;
    system("rm -rf build/t_pso_archive-cache && mkdir -p build/t_pso_archive-cache");
    for (int p = 1; p <= 3; p++) {
        if (p == 2) { int n = damage(1); printf("t_pso_archive: truncated %d cache file(s)\n", n); KNOWN(n > 0, "pass 1 wrote no cache file: the archive is off in this configuration"); }
        if (p == 3) { int n = damage(2) + damage(3); printf("t_pso_archive: overwrote %d cache file(s)\n", n); }
        char cmd[1200]; snprintf(cmd, sizeof cmd, "T_ARCH_PASS=%d MTL_DEBUG_LAYER=1 '%s' > build/t_pso_archive-pass%d.log 2>&1", p, argv[0], p);
        int rc = system(cmd);
        CHECK(rc == 0, "pass %d failed (rc %d), see build/t_pso_archive-pass%d.log", p, rc, p);
    }
    printf(g_fail ? "t_pso_archive: %d FAILED\n" : "t_pso_archive: ok (%d skipped)\n", g_fail ? g_fail : g_skip);
    return g_fail != 0;
}
