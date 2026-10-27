/* Placeholder graphics back end: answers the NVN bootstrap and names every NVN function the guest uses. Calling one ends the
 * run cleanly with the stage "nvn" recorded, so a boot test can report how far the guest got. Replaced by the real back end. */
#include "gfx.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern void *runtime_hle_wrap_trap(const char *name);
extern int (*runtime_trap_hook)(const char *name);

static const GfxHooks *g_hooks;

static int nvn_trap_hook(const char *name) {
    if (strncmp(name, "nvn", 3)) return 0;
    LOG("gfx", "guest called NVN function %s: the graphics back end is a stub", name);
    summary_nvn_missing(name);
    summary_stage("nvn");
    summary_write("partial", "boot reached graphics device initialization (first NVN call); graphics back end is a stub");
    _exit(0);
}
static void *get_proc_address(void *device, const char *name);
static void *stub_proc(const char *name) { return name && *name ? runtime_hle_wrap_trap(name) : NULL; }
static void *bootstrap_loader(const char *name) {
    LOG("gfx", "nvnBootstrapLoader(\"%s\")", name ? name : "(null)");
    runtime_peek("bootstrap");
    summary_stage("nvn-bootstrap");
    if (name && !strcmp(name, "nvnDeviceGetProcAddress") && g_hooks) return g_hooks->wrap_guest_callable(g_hooks->user, get_proc_address, name);
    return stub_proc(name);
}
static void *get_proc_address(void *device, const char *name) {
    (void)device;
    LOG("gfx", "nvnDeviceGetProcAddress(\"%s\")", name ? name : "(null)");
    return stub_proc(name);
}

static void nv_noop(void) { LOG("gfx", "nv:: graphics memory call accepted"); }
static const HostSymbol table[] = {
    {"_ZN2nv18InitializeGraphicsEPvm", nv_noop},
    /* SetGraphicsAllocator / SetGraphicsDevtoolsAllocator stay native: the SDK's own android:: code (nn::vi layers) allocates
     * through the callbacks they store. */
    {"nvnBootstrapLoader", bootstrap_loader},
    {"nvnDeviceGetProcAddress", get_proc_address},
};

int gfx_init(const GfxConfig *cfg, const GfxHooks *hooks) {
    (void)cfg;
    g_hooks = hooks;
    runtime_trap_hook = nvn_trap_hook;
    return 0;
}
const HostSymbol *gfx_hle_table(size_t *count) {
    *count = sizeof table / sizeof *table;
    return table;
}
int gfx_handle_fault(void *addr, int is_write, void *ucontext) {
    (void)addr;
    (void)is_write;
    (void)ucontext;
    return 0;
}
void gfx_guest_write_prepare(void *addr, size_t n) {
    (void)addr;
    (void)n;
}
uint64_t gfx_vsync_period_ns(void) { return 16666667; }
void gfx_set_nvn_version(unsigned major, unsigned minor) {
    (void)major;
    (void)minor;
}
void gfx_shutdown(void) {}
int gfx_screenshot(const char *p) {
    (void)p;
    return -1;
}
int gfx_screenshot_pending(void) { return 0; }
void gfx_set_hidden(int hidden) { (void)hidden; }
void gfx_compile_status(GfxCompileStatus *out) { memset(out, 0, sizeof *out); }
int gfx_texture_info(void *t, int *f, unsigned *w, unsigned *h) {
    (void)t;
    (void)f;
    (void)w;
    (void)h;
    return -1;
}
int gfx_texture_write_rgba(void *t, const void *p, size_t pitch, unsigned w, unsigned h) {
    (void)t;
    (void)p;
    (void)pitch;
    (void)w;
    (void)h;
    return -1;
}

void gfx_set_settings(const GfxSettings *s) { (void)s; }

/* Display pipeline (gfx/include/napple_display.h) has no back end in the stub build. */
#include "napple_display.h"
void napple_display_default(napple_display_settings *s) {
    memset(s, 0, sizeof *s);
    s->size = sizeof *s;
    s->scale = 1;
    s->supersample = 1;
    s->speed = 1;
}
void napple_display_set(const napple_display_settings *s) { (void)s; }
void napple_display_get(napple_display_settings *s) { napple_display_default(s); }
void napple_display_set_window(uint32_t w, uint32_t h, double hz) {
    (void)w;
    (void)h;
    (void)hz;
}
void napple_display_game_size(uint32_t bw, uint32_t bh, uint32_t *w, uint32_t *h) {
    *w = bw;
    *h = bh;
}
void napple_display_stats_text(char *out, unsigned cap) {
    if (cap) out[0] = 0;
}
