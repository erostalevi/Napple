#include <stdio.h>
#include <stdlib.h>
/* api_window.c - NVN windows (swapchains) and presentation. */
#include "gfx_mtl.h"
#include "timeline.h"

void gfx_api_link_window(void) {}

#define WIN_MAGIC MAGIC('W', 'I', 'N', '0')
#define TEX_MAGIC MAGIC('T', 'E', 'X', '0')
#define SYNC_MAGIC MAGIC('S', 'Y', 'N', 'C')
#define WB(b) ((win_builder_t *)(b))
#define CWB(b) ((const win_builder_t *)(b))
#define WN(w) ((win_t *)(w))
#define CWN(w) ((const win_t *)(w))
#define MAX_WIN_TEX 4

NVN(nvnWindowBuilderSetDevice, void, NVNwindowBuilder *b, NVNdevice *d) { WB(b)->dev = d; }
NVN(nvnWindowBuilderSetDefaults, void, NVNwindowBuilder *b) {
    NVNdevice *d = WB(b)->dev;
    memset(b, 0, sizeof(win_builder_t));
    WB(b)->dev = d;
    WB(b)->interval = 1;
}
NVN(nvnWindowBuilderSetNativeWindow, void, NVNwindowBuilder *b, NVNnativeWindow w) { WB(b)->native = w; }
NVN(nvnWindowBuilderSetTextures, void, NVNwindowBuilder *b, int n, NVNtexture *const *t) {
    if (n < 0) n = 0;
    if (n > MAX_WIN_TEX) { LOGW("window with %d textures: only %d are used", n, MAX_WIN_TEX); n = MAX_WIN_TEX; }
    WB(b)->ntex = n;
    for (int i = 0; i < n; i++) WB(b)->tex[i] = t[i];
}
NVN(nvnWindowBuilderSetPresentInterval, void, NVNwindowBuilder *b, int i) { WB(b)->interval = i; }
NVN(nvnWindowBuilderGetNativeWindow, NVNnativeWindow, const NVNwindowBuilder *b) { return CWB(b)->native; }
NVN(nvnWindowBuilderGetPresentInterval, int, const NVNwindowBuilder *b) { return CWB(b)->interval; }
NVN(nvnWindowBuilderSetNumActiveTextures, void, NVNwindowBuilder *b, int n) { WB(b)->active = n; }
NVN(nvnWindowBuilderGetNumActiveTextures, int, const NVNwindowBuilder *b) { return CWB(b)->active; }
NVN(nvnWindowBuilderGetDevice, const NVNdevice *, const NVNwindowBuilder *b) { return CWB(b)->dev; }
NVN(nvnWindowBuilderGetNumTextures, int, const NVNwindowBuilder *b) { return CWB(b)->ntex; }
NVN(nvnWindowBuilderGetTexture, const NVNtexture *, const NVNwindowBuilder *b, int i) { return (i >= 0 && i < CWB(b)->ntex) ? CWB(b)->tex[i] : NULL; }

NVN(nvnWindowInitialize, NVNboolean, NVNwindow *w, const NVNwindowBuilder *b) {
    win_t *o = WN(w);
    const win_builder_t *s = CWB(b);
    memset(o, 0, sizeof *o);
    if (s->ntex <= 0) { LOGE("WindowInitialize: no textures"); return 0; }
    o->magic = WIN_MAGIC;
    o->native = s->native;
    o->ntex = s->ntex;
    o->interval = s->interval > 0 ? s->interval : 1;
    o->active = s->active > 0 && s->active <= s->ntex ? s->active : s->ntex;
    for (int i = 0; i < s->ntex; i++) {
        o->tex[i] = s->tex[i];
        if (!s->tex[i] || ((const tex_t *)s->tex[i])->magic != TEX_MAGIC) { LOGE("WindowInitialize: invalid texture %d", i); o->magic = 0; return 0; }
    }
    gfx_mtl_window_attach(o);
    LOGI("window with %d textures (%ux%u)", o->ntex, ((const tex_t *)s->tex[0])->s.d.width, ((const tex_t *)s->tex[0])->s.d.height);
    for (int i = 0; i < s->ntex; i++) { const tex_t *t = (const tex_t *)s->tex[i]; LOGI("window texture %d: mem %p size %llu %ux%u format %#x flags %#x pool %p", i, (void *)t->s.mem, (unsigned long long)t->s.size, (unsigned)t->s.d.width, (unsigned)t->s.d.height, (unsigned)t->s.d.format, (unsigned)t->s.d.flags, (const void *)t->pool); }
    return 1;
}
NVN(nvnWindowFinalize, void, NVNwindow *w) { GFX_BLOCKING(); gfx_mtl_finish(); WN(w)->magic = 0; }
NVN(nvnWindowSetDebugLabel, void, NVNwindow *w, const char *l) { (void)w; (void)l; }
NVN(nvnWindowGetNativeWindow, NVNnativeWindow, const NVNwindow *w) { return CWN(w)->native; }
NVN(nvnWindowGetPresentInterval, int, const NVNwindow *w) { return CWN(w)->interval; }
NVN(nvnWindowSetPresentInterval, void, NVNwindow *w, int i) { WN(w)->interval = i > 0 ? i : 1; }
NVN(nvnWindowSetCrop, void, NVNwindow *w, int x, int y, int ww, int h) { int *c = WN(w)->crop; c[0] = x; c[1] = y; c[2] = ww; c[3] = h; }
NVN(nvnWindowGetCrop, void, const NVNwindow *w, NVNrectangle *r) { const int *c = CWN(w)->crop; r->x = c[0]; r->y = c[1]; r->width = c[2]; r->height = c[3]; }
NVN(nvnWindowSetNumActiveTextures, void, NVNwindow *w, int n) { if (n > 0 && n <= WN(w)->ntex) WN(w)->active = n; }
NVN(nvnWindowGetNumActiveTextures, int, const NVNwindow *w) { return CWN(w)->active; }
NVN(nvnWindowGetNumTextures, int, const NVNwindow *w) { return CWN(w)->ntex; }

/* Acquire the next image. The image may still be on screen: the sync signals once it was shown (display of its previous present done). */
NVN(nvnWindowAcquireTexture, NVNwindowAcquireTextureResult, NVNwindow *w, NVNsync *s, int *index) {
    win_t *o = WN(w);
    if (o->magic != WIN_MAGIC) return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_NATIVE_ERROR;
    int i = o->next % o->active;
    o->next = (o->next + 1) % o->active;
    sync_t *sy = (sync_t *)s;
    if (sy) { sy->magic = SYNC_MAGIC; sy->kind = 1; sy->cell = &o->displayed[i]; sy->seq = o->ticket[i]; }
    *index = i;
    return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
NVN(nvnQueueAcquireTexture, NVNqueueAcquireTextureResult, NVNqueue *q, NVNwindow *w, int *index) {
    (void)q;
    win_t *o = WN(w);
    if (o->magic != WIN_MAGIC) return NVN_QUEUE_ACQUIRE_TEXTURE_RESULT_NATIVE_ERROR;
    int i = o->next % o->active;
    o->next = (o->next + 1) % o->active;
    *index = i;
    return NVN_QUEUE_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}

NVN(nvnQueuePresentTexture, void, NVNqueue *q, NVNwindow *w, int index) {
    queue_t *qo = (queue_t *)q;
    win_t *o = WN(w);
    if (o->magic != WIN_MAGIC || index < 0 || index >= o->ntex) { LOG_ONCE(NAPPLE_LOG_WARN, "QueuePresentTexture with an invalid window or index %d", index); return; }
    const tex_t *t = (const tex_t *)o->tex[index];
    o->ticket[index]++;
    { static int pl = -1, n; if (pl < 0) pl = getenv("NAPPLE_PRESENTLOG") ? atoi(getenv("NAPPLE_PRESENTLOG")) : 0; if (pl && (int)o->presents >= pl && n++ < 12) { fprintf(stderr, "[presentlog] present index %d of %d mem %p %ux%u fmt %#x; window textures:", index, o->ntex, (void *)(uintptr_t)t->s.mem, (unsigned)t->s.d.width, (unsigned)t->s.d.height, (unsigned)t->s.d.format); for (int i = 0; i < o->ntex; i++) fprintf(stderr, " %p", (void *)(uintptr_t)((const tex_t *)o->tex[i])->s.mem); fputc('\n', stderr); } }
    uint64_t seq = gfx_mtl_present(o, index, &t->s, (uint32_t)o->interval);
    atomic_store(&qo->last_seq, seq);
    gfx_notify_present(o->presents++, (uint32_t)o->interval);
}
