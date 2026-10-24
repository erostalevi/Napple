/* blob.c - registry of shader blobs keyed by content hash. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../gfx_mtl.h"
#include "blob.h"

#define TAB 65536
static shader_blob *g_tab[TAB];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

const shader_blob *shader_blob_find(uint64_t hash) {
    if (!hash) return NULL;
    pthread_mutex_lock(&g_mu);
    uint32_t i = (uint32_t)hash & (TAB - 1);
    const shader_blob *r = NULL;
    for (uint32_t n = 0; n < TAB && g_tab[i]; n++, i = (i + 1) & (TAB - 1)) if (g_tab[i]->hash == hash) { r = g_tab[i]; break; }
    pthread_mutex_unlock(&g_mu);
    return r;
}

uint64_t gfx_shader_register(int stage, uint64_t code_addr, const uint8_t *control) {
    uint32_t const_off, const_size;
    memcpy(&const_off, control + 0x700, 4);
    memcpy(&const_size, control + 0x6FC, 4);
    if (!const_off || const_off > (16u << 20) || const_size > 0x10000) { LOGE("shader control section has implausible bounds (%#x/%#x)", const_off, const_size); return 0; }
    const uint8_t *code = (const uint8_t *)(uintptr_t)code_addr;
    uint64_t h = fnv1a(code, const_off, FNV_INIT);
    h = fnv1a(code + const_off, const_size, h);
    h = fnv1a(&stage, sizeof stage, h);
    if (!h) h = 1;
    if (shader_blob_find(h)) return h;
    shader_blob *b = calloc(1, sizeof *b);
    if (!b) return 0;
    b->hash = h; b->stage = stage;
    b->code = malloc(const_off); b->code_size = const_off; memcpy(b->code, code, const_off);
    b->consts = malloc(const_size ? const_size : 16); b->const_size = const_size; memset(b->consts, 0, const_size ? const_size : 16);
    memcpy(b->consts, code + const_off, const_size);
    if (stage == 5) memcpy(b->local, control + 0x748, 12);
    if (stage == 5 && g_gfx.log_level >= NAPPLE_LOG_DEBUG) {
        const uint32_t *w = (const uint32_t *)control;
        for (int i = 0x6F0 / 4; i < 0x780 / 4; i += 4) LOGD("compute %016llx control +%#x: %08x %08x %08x %08x", (unsigned long long)h, i * 4, w[i], w[i + 1], w[i + 2], w[i + 3]);
    }
    pthread_mutex_lock(&g_mu);
    uint32_t i = (uint32_t)h & (TAB - 1);
    uint32_t n = 0;
    while (g_tab[i] && n++ < TAB) i = (i + 1) & (TAB - 1);
    if (n >= TAB) { pthread_mutex_unlock(&g_mu); LOGE("shader blob table full"); free(b->code); free(b->consts); free(b); return 0; }
    g_tab[i] = b;
    pthread_mutex_unlock(&g_mu);
    if (stage != 5) mtl_pipe_prewarm(h);
    return h;
}
