/* Mods at run time: see mods_runtime.h. Cheat opcodes live in mods_cheats.c; this file owns the files, the guest memory view and the config mapping. */
#include "mods_runtime.h"
#include "json.h"
#include "mods_cheats.h"
#include "napple.h"
#include "napple_frontend.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static ModsCheats *g_cheats;
static JsonNode *g_cfg_json;
static unsigned g_frames, g_logged_fail;
static uint8_t *g_main_base;
static uint64_t g_main_size, g_main_writable;
static int g_main_set;

static char *read_file(const char *dir, const char *rel, size_t *len) {
    char p[4096];
    snprintf(p, sizeof p, "%s/mods/%s", dir, rel);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (8 << 20)) {
        fclose(f);
        return NULL;
    }
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) {
        b[n] = 0;
        *len = (size_t)n;
    }
    return b;
}

static void init_once(void) {
    if (getenv("NAPPLE_NO_MODS") || !g_cfg.data_dir) return;
    size_t n = 0;
    char *t = read_file(g_cfg.data_dir, "cheats.txt", &n);
    if (t) {
        char err[160] = "";
        g_cheats = mods_cheats_parse(t, n, err, sizeof err);
        if (!g_cheats) LOG("mods", "cheats.txt rejected: %s", err);
        else {
            unsigned run = 0;
            for (size_t i = 0; i < g_cheats->n; i++) {
                if (g_cheats->c[i].unsupported) LOG("mods", "cheat [%s] not run: %s", g_cheats->c[i].name, g_cheats->c[i].unsupported);
                else run++;
            }
            LOG("mods", "cheats: %u of %zu will run each frame", run, g_cheats->n);
        }
        free(t);
    }
    t = read_file(g_cfg.data_dir, "config.json", &n);
    if (t) {
        g_cfg_json = json_parse(t, n);
        if (!g_cfg_json) LOG("mods", "config.json rejected (malformed)");
        else LOG("mods", "config.json loaded");
        free(t);
    }
}
void mods_runtime_init(void) { pthread_once(&g_once, init_once); }

void mods_runtime_set_main(uint8_t *base, uint64_t size, uint64_t writable_from) {
    g_main_base = base;
    g_main_size = size;
    g_main_writable = writable_from;
    g_main_set = 1;
}

/* A region-relative address as a host pointer: NULL when outside the guest or (for Main) writes into code. */
static void *resolve(int region, uint64_t addr, int width, int for_write) {
    uint64_t abs = addr;
    switch (region) {
    case MC_MAIN: {
        uint8_t *base = g_main_base;
        uint64_t size = g_main_size, writable = g_main_writable;
        if (!g_main_set) {
            const Module *m = module_by_name("main");
            if (!m) return NULL;
            base = m->base;
            size = m->size;
            writable = m->data_off; /* text and rodata are read-only in the rewritten layout */
        }
        if (addr > size || (uint64_t)width > size - addr || (for_write && addr < writable)) return NULL;
        return base + addr;
    }
    case MC_HEAP: abs = memory_heap_base() + addr; break;
    case MC_ALIAS: abs = memory_alias_base() + addr; break;
    case MC_ASLR: abs = memory_aslr_base() + addr; break;
    default: break;
    }
    return guest_ptr(abs, (uint64_t)width);
}
static int guest_read(void *ctx, int region, uint64_t addr, int width, uint64_t *out) {
    (void)ctx;
    const void *p = resolve(region, addr, width, 0);
    if (!p) return 0;
    uint64_t v = 0;
    memcpy(&v, p, (size_t)width); /* little endian host */
    *out = v;
    return 1;
}
static int guest_write(void *ctx, int region, uint64_t addr, int width, uint64_t value) {
    (void)ctx;
    void *p = resolve(region, addr, width, 1);
    if (!p) return 0;
    guest_write_prepare((uint64_t)(uintptr_t)p, (size_t)width);
    memcpy(p, &value, (size_t)width);
    return 1;
}

void mods_cheats_frame(void) {
    mods_runtime_init();
    if (!g_cheats) return;
    static const McMem mem = {guest_read, guest_write};
    McStats st = {0};
    mods_cheats_run_all(g_cheats, &mem, NULL, &st);
    if (g_frames++ == 0) LOG("mods", "cheats: first frame ran, %u store(s), %u failed access(es)", st.stores, st.failed);
    if ((st.failed || st.aborted) && g_logged_fail++ < 3) LOG("mods", "cheats: %u failed access(es)%s", st.failed, st.aborted ? ", step budget exhausted" : "");
}

int mods_config_apply_settings(napple_settings *s) {
    mods_runtime_init();
    if (!g_cfg_json || g_cfg_json->type != J_OBJ || !s) return 0;
    int changed = 0;
    const JsonNode *rs = json_get(g_cfg_json, "renderScale");
    if (rs && rs->type == J_NUM && rs->num >= 0.25 && rs->num <= 8.0) {
        s->render_mode = 1;
        s->internal_scale_percent = (int)lround(rs->num * 100.0);
        changed = 1;
    }
    const char *as = json_str(g_cfg_json, "aspect", NULL);
    if (as && strcmp(as, "native")) {
        unsigned w = 0, h = 0;
        if (sscanf(as, "%u:%u", &w, &h) == 2 && w && h) {
            if (w == 16 && h == 9) s->aspect = 1;
            else if (w == 4 && h == 3) s->aspect = 2;
            else { /* free window, the game is told the window's shape: size the window to the ratio */
                s->aspect = 6;
                s->output_height = 720;
                s->output_width = (int)((720ull * w + h / 2) / h);
            }
            changed = 1;
        }
    }
    const char *fm = json_str(g_cfg_json, "fpsMode", NULL);
    if (fm) {
        if (!strcmp(fm, "default")) s->fps_mode = 0;
        else if (!strcmp(fm, "unlocked")) s->fps_mode = 3;
        else if (!strcmp(fm, "30") || !strcmp(fm, "60")) {
            s->fps_mode = 2;
            s->fps_cap = atoi(fm);
        } else fm = NULL;
        if (fm) changed = 1;
    }
    if (changed) LOG("mods", "config.json applied to the settings of this run (render scale / aspect / fps mode)");
    return changed;
}

int mods_fix_enabled(const char *name, int dflt) {
    mods_runtime_init();
    const JsonNode *f = g_cfg_json ? json_get(g_cfg_json, "fixes") : NULL;
    const JsonNode *v = f && f->type == J_OBJ ? json_get(f, name) : NULL;
    return v && v->type == J_BOOL ? (int)v->num : dflt;
}
