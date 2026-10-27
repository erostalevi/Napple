/* napple-runtime <data-dir>: runs a rewritten game from its data directory (see docs/architecture/module-format.md). */
#include "napple_display.h"
#include "napple.h"
uint8_t *memory_a32_base(void);
void a32_process_init(const char *data_dir);
int a32_detect_nvn_version(unsigned *major, unsigned *minor);
_Noreturn void a32_boot(uint32_t main_handle);
#include "frontend.h"
#include "gfx.h"
#include "ipc.h"
#include "json.h"
#include "movie.h"
#include "ios_platform.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

RuntimeConfig g_cfg;
void thread_env_init_keys(void);
void dispatch_build_index(void);
void svc_kernel_register(void);

static const char USAGE[] = "usage: napple-runtime <data-dir> [options]\n"
                            "  --headless          no window (graphics back end runs without a layer)\n"
                            "  --seconds N         stop after N seconds (boot tests)\n"
                            "  --trace FILE        JSON-lines trace of svc/IPC events (also NAPPLE_TRACE)\n"
                            "  --summary FILE      write the compatibility stage file runtime-boot.json there\n"
                            "  --save-root DIR     user data root (default ~/Library/Application Support/Napple)\n"
                            "  --svc-trace         log every svc\n"
                            "  -v                  verbose log (also NAPPLE_VERBOSE=1)\n";

static void load_identity(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/manifest.json", dir);
    FILE *f = fopen(path, "rb");
    if (!f) runtime_fail("%s not found: is this a game data directory?", path);
    char *buf = malloc(1 << 20);
    size_t n = fread(buf, 1, (1 << 20) - 1, f);
    fclose(f);
    JsonNode *root = json_parse(buf, n);
    const char *t = json_str(root, "title_id", NULL), *b = json_str(root, "build_id", NULL);
    if (!t || !b) runtime_fail("%s: title_id/build_id missing", path);
    snprintf(g_cfg.title_id, sizeof g_cfg.title_id, "%.16s", t);
    snprintf(g_cfg.build_id, sizeof g_cfg.build_id, "%.16s", b);
    g_program_id = strtoull(g_cfg.title_id, NULL, 16);
    json_free(root);
    free(buf);
}

/* Optional per-game settings that are data, not code: <data-dir>/game.json {"title_name":..., "nvn_major":..., "nvn_minor":...}. */
static void load_game_config(const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/game.json", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char *buf = malloc(1 << 16);
    size_t n = fread(buf, 1, (1 << 16) - 1, f);
    fclose(f);
    JsonNode *root = json_parse(buf, n);
    if (root) {
        snprintf(g_cfg.title_name, sizeof g_cfg.title_name, "%s", json_str(root, "title_name", ""));
        g_cfg.nvn_major = (unsigned)json_num(root, "nvn_major", 0);
        g_cfg.nvn_minor = (unsigned)json_num(root, "nvn_minor", 0);
        /* Render target scaling escape hatches (docs/graphics-and-speed-settings.md): "rscale": 0 never scales this game's render targets, "rscale_min_div": a render
         * target counts as screen sized when its width >= output width / this (default 8). */
        {
            napple_display_settings d;
            napple_display_get(&d);
            if (json_get(root, "rscale")) d.rscale = json_num(root, "rscale", 1) != 0;
            if (json_get(root, "rscale_min_div")) d.rscale_min_div = (uint32_t)json_num(root, "rscale_min_div", 8);
            napple_display_set(&d);
        }
        json_free(root);
    } else LOG("main", "%s: malformed, ignored", path);
    free(buf);
}

/* The NVN API version the game was built against: games compare nvnDeviceGetInteger(API_MAJOR/MINOR) with build-time constants
 * (`cmp w,#major; b.ne fail; ldr w,[sp,..]; cmp w,#minor; b.lt fail`) and skip graphics start-up on a mismatch. When game.json does not say, look for that
 * pattern in the main module (major 40..70, minor 5..1000); no match keeps the back end default. */
static void detect_nvn_version(void) {
    const Module *m = module_by_name("main");
    if (!m) return;
    const uint32_t *w = (const uint32_t *)m->base;
    size_t n = m->text_end / 4;
    for (size_t i = 0; i + 7 < n; i++) {
        if ((w[i] & 0xffc0001f) != 0x7100001f) continue;
        unsigned major = (w[i] >> 10) & 0xfff;
        if (major < 40 || major > 70 || (w[i + 1] & 0xff00001f) != 0x54000001) continue;
        for (int k = 2; k <= 3; k++)
            if ((w[i + k] & 0xffc00000) == 0xb9400000 && (w[i + k + 1] & 0xffc0001f) == 0x7100001f && (w[i + k + 2] & 0xff00001f) == 0x5400000b) {
                unsigned minor = (w[i + k + 1] >> 10) & 0xfff;
                if (minor < 5 || minor > 1000) continue; /* 0x138 = 312 in Animal Crossing (NVN 53.312) */
                g_cfg.nvn_major = major;
                g_cfg.nvn_minor = minor;
                gfx_set_nvn_version(major, minor); /* gfx_init already ran with the default */
                LOG("main", "NVN API version %u.%u detected in the main module", major, minor);
                return;
            }
    }
}

/* ---- guest start ----------------------------------------------------------------------------------------------- */
static void run_init(const Module *m) {
    LOG("boot", "running %s initializers (DT_INIT %#llx, %llu init_array entries)", m->name, (unsigned long long)m->dt_init,
        (unsigned long long)(m->init_array_size / 8));
    trace_event("module_init", "\"module\":\"%s\"", m->name);
    if (m->dt_init) {
        ((void (*)(void))(m->base + m->dt_init))();
        return;
    }
    for (uint64_t i = 0; i < m->init_array_size / 8; i++) {
        uint64_t fn;
        memcpy(&fn, m->base + m->init_array + i * 8, 8);
        if (fn && fn != ~0ull) ((void (*)(void))(uintptr_t)fn)();
    }
}
/* nn::init::Start(thread handle, 0, A, B) is handed two callbacks by the loader: A runs the SDK module's initializers right
 * after nn::os is up, B runs every other module's once nninitStartup has set up the heap, just before nnMain. */
static void cb_sdk_init(void) {
    const Module *m = module_by_name("sdk");
    if (m) run_init(m);
    summary_stage("sdk-init");
}
static void cb_game_init(void) {
    for (unsigned i = 0; i < g_nmodules; i++)
        if (strcmp(g_modules[i].name, "sdk")) run_init(&g_modules[i]);
    summary_stage("module-init");
}
/* SDK 15 variant Start(handle, 0, ready, inits, finis) */
static void cb_ready(void) { LOG("boot", "loader callback: modules linked"); }
static void cb_all_init(void) {
    for (unsigned i = 0; i < g_nmodules; i++) run_init(&g_modules[i]);
    summary_stage("module-init");
}
static void cb_finis(void) { runtime_fail("guest ran module finalizers (exit path not implemented)"); }

static GfxConfig gc;
static void boot(void) {
    gc.metal_layer = frontend_metal_layer();
    gc.nvn_major = g_cfg.nvn_major;
    gc.nvn_minor = g_cfg.nvn_minor;
    {
        static char cache[1200];
        snprintf(cache, sizeof cache, "%s/%s/cache", g_cfg.save_root, g_cfg.title_id);
        mkdir(g_cfg.save_root, 0755);
        char t[1100];
        snprintf(t, sizeof t, "%s/%s", g_cfg.save_root, g_cfg.title_id);
        mkdir(t, 0755);
        mkdir(cache, 0755);
        gc.cache_dir = cache;
    }
    gc.gmem_base = memory_heap_base();
    gc.gmem_size = 0; /* unused: global memory is addressed by GPU virtual address (see gfx mtl_replay.m) */
    if (gfx_init(&gc, runtime_gfx_hooks())) runtime_fail("graphics back end failed to initialize");
    LOG("boot", "graphics back end initialised (%s)", gc.headless ? "headless" : "window");
    {
        extern void frontend_gfx_ready(void);
        frontend_gfx_ready();
    }
    frontend_wait_shader_prewarm(gc.cache_dir);
    runtime_thread_init();
    uint32_t main_handle = threads_register_main();
    if (memory_a32_base()) a32_boot(main_handle); /* AArch32 title: replays rtld's startup sequence on the interpreter (never returns) */
    void *start2 = module_find_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    void *start3 = module_find_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_");
    summary_stage("start");
    if (start2) {
        LOG("boot", "entering nn::init::Start (2 callbacks), main thread handle %#x", main_handle);
        ((void (*)(uint64_t, uint64_t, void *, void *))start2)(main_handle, 0, runtime_hle_wrap_named(cb_sdk_init, "<sdk init>"),
                                                               runtime_hle_wrap_named(cb_game_init, "<game init>"));
    } else if (start3) {
        LOG("boot", "entering nn::init::Start (3 callbacks), main thread handle %#x", main_handle);
        ((void (*)(uint64_t, uint64_t, void *, void *, void *))start3)(main_handle, 0, runtime_hle_wrap_named(cb_ready, "<ready>"),
                                                                       runtime_hle_wrap_named(cb_all_init, "<inits>"),
                                                                       runtime_hle_wrap_named(cb_finis, "<finis>"));
    } else runtime_fail("no nn::init::Start export found in the modules");
    runtime_fail("nn::init::Start returned");
}

#ifdef NAPPLE_LIB
int napple_applet_main(int argc, char **argv) {
#else
int main(int argc, char **argv) {
#endif
    setvbuf(stderr, NULL, _IOLBF, 0);
    const char *trace = NULL, *summary = NULL, *save_root = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--headless")) g_cfg.headless = 1;
        else if (!strcmp(a, "--seconds") && i + 1 < argc) g_cfg.seconds = atoi(argv[++i]);
        else if (!strcmp(a, "--trace") && i + 1 < argc) trace = argv[++i];
        else if (!strcmp(a, "--summary") && i + 1 < argc) summary = argv[++i];
        else if (!strcmp(a, "--save-root") && i + 1 < argc) save_root = argv[++i];
        else if (!strcmp(a, "--svc-trace")) svc_trace_all = 1;
        else if (!strcmp(a, "-v")) setenv("NAPPLE_VERBOSE", "1", 1);
        else if (a[0] != '-' && !g_cfg.data_dir) g_cfg.data_dir = a;
        else if (NAPPLE_IOS) continue; /* UIKit / Xcode / devicectl add launch arguments of their own */
        else {
            fputs(USAGE, stderr);
            return 2;
        }
    }
#if NAPPLE_IOS
    if (!g_cfg.data_dir) g_cfg.data_dir = ios_default_data_dir();
    if (!save_root) save_root = ios_default_save_root();
#endif
    if (!g_cfg.data_dir) {
        fputs(USAGE, stderr);
        return 2;
    }
    if (trace) setenv("NAPPLE_TRACE", trace, 1);
    log_init();
    if (getenv("NAPPLE_HLE_TRACE")) runtime_hle_trace_set(atoi(getenv("NAPPLE_HLE_TRACE")));
    if (getenv("NAPPLE_SVC_TRACE")) svc_trace_all = 1;
    summary_path = summary ? summary : getenv("NAPPLE_SUMMARY");
    static char root[1024];
    if (!save_root) {
        snprintf(root, sizeof root, "%s/Library/Application Support/Napple", getenv("HOME") ? getenv("HOME") : ".");
        save_root = root;
    }
    g_cfg.save_root = save_root;
#if NAPPLE_IOS
    ios_install_fonts(g_cfg.data_dir, save_root);
#endif
    if (sysconf(_SC_PAGESIZE) != 0x4000) runtime_fail("expected 16 KiB host pages");
    load_identity(g_cfg.data_dir);
    load_game_config(g_cfg.data_dir);
    summary_set_title(g_cfg.title_id, g_cfg.build_id);
    LOG("main", "title %s build %s data %s", g_cfg.title_id, g_cfg.build_id, g_cfg.data_dir);

    char path[4096];
    snprintf(path, sizeof path, "%s/main.npdm", g_cfg.data_dir);
    if (npdm_load(path))
        LOG("main", "npdm: %s, main thread priority %u core %u stack %#x, %u services, %u svcs allowed", g_npdm.name, g_npdm.main_priority, g_npdm.main_core,
            g_npdm.main_stack_size, g_npdm.nservices, g_npdm.nsvc_allowed);
    else LOG("main", "no usable main.npdm in the data directory: no service/svc access checks");

    thread_env_init_keys();
    signals_install();
    svc_kernel_register();
    svc_threads_register();
    svc_memory_register();
    svc_ipc_register();
    int a32_title = g_npdm.valid && !g_npdm.is_64bit;
    if (a32_title) a32_process_init(g_cfg.data_dir);
    else memory_init();
    {
        size_t n;
        const HostSymbol *t = gfx_hle_table(&n);
        host_symbols_register(t, n);
    }
    movie_register_overrides();
    {
        extern void time_register_overrides(void);
        time_register_overrides();
    }
    if (!a32_title) modules_load(g_cfg.data_dir);
    {
        extern void watch_install(void);
        watch_install();
    }
    if (!g_cfg.nvn_major && !a32_title) detect_nvn_version();
    if (!g_cfg.nvn_major && a32_title) {
        unsigned ma, mi;
        if (a32_detect_nvn_version(&ma, &mi)) {
            g_cfg.nvn_major = ma;
            g_cfg.nvn_minor = mi;
            LOG("main", "NVN API version %u.%u detected in the AArch32 main module", ma, mi);
        }
    }
    dispatch_build_index();
    services_init();
    {
        extern void service_register_ro(void);
        service_register_ro();
    } /* ldr:ro / ro:1 belong to the loader (ro.c) */
    if (getenv("NAPPLE_WAIT_SECS")) {
        LOG("main", "waiting %s s for a debugger (pid %d)", getenv("NAPPLE_WAIT_SECS"), getpid());
        sleep((unsigned)atoi(getenv("NAPPLE_WAIT_SECS")));
    }

    gc.title_id = g_cfg.title_id;
    gc.build_id = g_cfg.build_id;
    gc.headless = g_cfg.headless;
    gc.output_width = 1280;
    gc.output_height = 720;
    if (g_cfg.headless) { /* NAPPLE_OUTPUT_SIZE=WxH: the size of the headless "window" (drawable), to exercise other screen shapes and sizes */
        const char *e = getenv("NAPPLE_OUTPUT_SIZE");
        int w = 0, h = 0;
        if (e && sscanf(e, "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64 && w <= 16384 && h <= 16384) {
            gc.output_width = w;
            gc.output_height = h;
        }
        napple_display_set_window((uint32_t)gc.output_width, (uint32_t)gc.output_height, 60);
    }
    return frontend_run(boot, g_cfg.headless, g_cfg.seconds);
}
