/* blob.c - registry of shader blobs keyed by content hash. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../gfx_mtl.h"
#include "blob.h"
#include "control.h"

#define TAB 65536
static shader_blob *g_tab[TAB];
static void control_apply(int stage, uint64_t h, const uint8_t *control, const uint8_t *code);
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

const shader_blob *shader_blob_find(uint64_t hash) {
    if (!hash) return NULL;
    pthread_mutex_lock(&g_mu);
    uint32_t i = (uint32_t)hash & (TAB - 1);
    const shader_blob *r = NULL;
    for (uint32_t n = 0; n < TAB && g_tab[i]; n++, i = (i + 1) & (TAB - 1))
        if (g_tab[i]->hash == hash) {
            r = g_tab[i];
            break;
        }
    pthread_mutex_unlock(&g_mu);
    return r;
}

uint64_t gfx_shader_register(int stage, uint64_t code_addr, const uint8_t *control) {
    uint32_t const_off, const_size;
    memcpy(&const_off, control + 0x700, 4);
    memcpy(&const_size, control + 0x6FC, 4);
    if (!const_off || const_off > (16u << 20) || const_size > 0x10000) {
        LOGE("shader control section has implausible bounds (%#x/%#x)", const_off, const_size);
        return 0;
    }
    const uint8_t *code = (const uint8_t *)(uintptr_t)code_addr;
    uint64_t h = fnv1a(code, const_off, FNV_INIT);
    h = fnv1a(code + const_off, const_size, h);
    h = fnv1a(&stage, sizeof stage, h);
    if (!h) h = 1;
    if (shader_blob_find(h)) return h;
    shader_blob *b = calloc(1, sizeof *b);
    if (!b) return 0;
    b->hash = h;
    b->stage = stage;
    b->code = malloc(const_off);
    b->code_size = const_off;
    memcpy(b->code, code, const_off);
    b->consts = malloc(const_size ? const_size : 16);
    b->const_size = const_size;
    memset(b->consts, 0, const_size ? const_size : 16);
    memcpy(b->consts, code + const_off, const_size);
    if (stage == 5) memcpy(b->local, control + 0x748, 12);
    if (stage == 5 && g_gfx.log_level >= NAPPLE_LOG_DEBUG) {
        const uint32_t *w = (const uint32_t *)control;
        for (int i = 0x6F0 / 4; i < 0x780 / 4; i += 4)
            LOGD("compute %016llx control +%#x: %08x %08x %08x %08x", (unsigned long long)h, i * 4, w[i], w[i + 1], w[i + 2], w[i + 3]);
    }
    pthread_mutex_lock(&g_mu);
    uint32_t i = (uint32_t)h & (TAB - 1);
    uint32_t n = 0;
    while (g_tab[i] && n++ < TAB) i = (i + 1) & (TAB - 1);
    if (n >= TAB) {
        pthread_mutex_unlock(&g_mu);
        LOGE("shader blob table full");
        free(b->code);
        free(b->consts);
        free(b);
        return 0;
    }
    g_tab[i] = b;
    pthread_mutex_unlock(&g_mu);
    control_apply(stage, h, control, code);
    if (stage != 5) mtl_pipe_prewarm(h);
    return h;
}

/* What the control section says beyond the code bounds: the transform feedback layout of a vertex / geometry / tess eval program and the tessellation
 * parameters of a tess eval program go to the registries of stages.h (an explicit gfx_*_set_* registration made later still wins: it overwrites).
 * NAPPLE_GFX_CTL_DUMP=<dir> writes every registered stage's control section (<dir>/s<stage>-<hash>.ctl, its own length) and code area (.code) for
 * offline study; the first geometry / tess control / tess eval program a title creates is logged so the boot logs tell whether a title has them. */
static void control_apply(int stage, uint64_t h, const uint8_t *control, const uint8_t *code) {
    static const char *dump = (const char *)1;
    if (dump == (const char *)1) dump = getenv("NAPPLE_GFX_CTL_DUMP");
    gfx_control_info ci;
    gfx_control_parse(control, 0, &ci);
    if (dump && dump[0]) {
        char path[1024];
        snprintf(path, sizeof path, "%s/s%d-%016llx.ctl", dump, stage, (unsigned long long)h);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(control, 1, ci.length ? ci.length : 0x880, f); fclose(f); }
        snprintf(path, sizeof path, "%s/s%d-%016llx.code", dump, stage, (unsigned long long)h);
        f = fopen(path, "wb");
        if (f) { fwrite(code, 1, ci.const_off + ci.const_size, f); fclose(f); }
    }
    if (stage == 2) LOG_ONCE(NAPPLE_LOG_INFO, "title creates geometry programs (first: %016llx)", (unsigned long long)h);
    if (stage == 3) LOG_ONCE(NAPPLE_LOG_INFO, "title creates tess control programs (first: %016llx)", (unsigned long long)h);
    if (stage == 4) LOG_ONCE(NAPPLE_LOG_INFO, "title creates tess eval programs (first: %016llx)", (unsigned long long)h);
    if (stage == 0 || stage == 2 || stage == 4) {
        gfx_xfb_layout xl;
        if (gfx_control_xfb(control, 0, &xl)) {
            gfx_xfb_set_layout(h, &xl);
            LOG_ONCE(NAPPLE_LOG_INFO, "transform feedback layout read from a control section (stage %d program %016llx, %u entries)", stage, (unsigned long long)h, xl.count);
        }
    }
    if (stage == 4) {
        gfx_tess_params tp;
        if (gfx_control_tess(control, 0, &tp)) {
            gfx_tess_set_params(h, &tp);
            LOG_ONCE(NAPPLE_LOG_INFO, "tessellation parameters read from a control section (program %016llx: domain %u spacing %u winding %u)", (unsigned long long)h, tp.domain,
                     tp.spacing, tp.winding);
        }
    }
}
