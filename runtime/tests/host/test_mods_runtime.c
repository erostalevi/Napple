/* mods_runtime.c on the host: cheats against a fake guest (heap/main) and config.json -> settings. Portable. */
#include "../check.h"
#include "mods_runtime.h"
#include "napple.h"
#include "napple_frontend.h"
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

/* the few runtime symbols mods_runtime.c needs: guest memory is host memory, the main module is injected with mods_runtime_set_main */
RuntimeConfig g_cfg;
void log_line(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
void *guest_ptr(uint64_t addr, uint64_t size) { return addr < 0x1000 || addr + size < addr ? NULL : (void *)(uintptr_t)addr; }
void guest_write_prepare(uint64_t addr, size_t size) { (void)addr; (void)size; }
uint64_t memory_heap_base(void) { return 0; }
uint64_t memory_alias_base(void) { return 0; }
uint64_t memory_aslr_base(void) { return 0; }
void napple_settings_default(napple_settings *s) { memset(s, 0, sizeof *s); } /* frontend/src/settings.c is macOS-only (clock_gettime_nsec_np) */
const Module *module_by_name(const char *name) { (void)name; return NULL; }

static void put(const char *path, const char *body) {
    FILE *f = fopen(path, "wb");
    fputs(body, f);
    fclose(f);
}
int main(void) {
    char dir[] = "/tmp/napple-modsrt-XXXXXX";
    if (!mkdtemp(dir)) return 2;
    char p[4096];
    snprintf(p, sizeof p, "%s/mods", dir);
    mkdir(p, 0755);
    static uint8_t mainmem[0x1000];
    mods_runtime_set_main(mainmem, sizeof mainmem, 0x800); /* [0x800, ...) is data */
    snprintf(p, sizeof p, "%s/mods/cheats.txt", dir);
    put(p, "// mod demo.mod\n[Data store]\n04000000 00000900 0000BEEF\n[Text store]\n04000000 00000100 0000BEEF\n[If]\n14050000 00000900 0000BEEF\n01000000 00000A00 00000042\n20000000\n[Unsupported]\nA0000000\n");
    snprintf(p, sizeof p, "%s/mods/config.json", dir);
    put(p, "{\"renderScale\":1.5,\"aspect\":\"21:9\",\"fpsMode\":\"60\",\"fixes\":{\"skipIntroLogos\":true,\"x\":false}}");
    g_cfg.data_dir = dir;
    mods_cheats_frame();
    CHECK(mainmem[0x900] == 0xEF && mainmem[0x901] == 0xBE, "data store lands");
    CHECK(mainmem[0x100] == 0 && mainmem[0x101] == 0, "stores into text are refused");
    CHECK(mainmem[0xA00] == 0x42, "a later cheat sees the store of an earlier one in the same frame");
    mainmem[0x900] = 0;
    mainmem[0xA00] = 0;
    mods_cheats_frame();
    CHECK(mainmem[0xA00] == 0x42, "second frame runs again");

    napple_settings s;
    napple_settings_default(&s);
    CHECK(mods_config_apply_settings(&s) == 1, "config applies");
    CHECK(s.render_mode == 1 && s.internal_scale_percent == 150, "render scale %d%%", s.internal_scale_percent);
    CHECK(s.aspect == 6 && s.output_height == 720 && s.output_width == 1680, "21:9 window %dx%d", s.output_width, s.output_height);
    CHECK(s.fps_mode == 2 && s.fps_cap == 60, "fps mode");
    CHECK(mods_fix_enabled("skipIntroLogos", 0) == 1 && mods_fix_enabled("x", 1) == 0 && mods_fix_enabled("unknown", 7) == 7, "fixes");
    printf("test_mods_runtime: %d checks, %d failed\n", g_checked, g_failed);
    return g_failed != 0;
}
