/* api_objtrace.c - NAPPLE_GFX_OBJTRACE=1: how large a title's NVN objects really are (the evidence behind the minimum sizes in obj.h).
 *
 * A title allocates each NVN object with its SDK's sizeof, which may be smaller than nvnapi/nvn.h's reserved size (Animal Crossing's NVNprogram is followed
 * by other data at +0x88). The distance from an object to the next NVN object in memory is an upper bound of its size in that title; in arrays it is exactly
 * the size. Objects with a lifetime (Initialize .. Finalize) are kept in an address-ordered set: each Initialize gives the gap to the live neighbour above
 * (for the new object) and below (for that neighbour). Builders, views and state objects have no lifetime (usually on the stack): they are paired with the
 * objects the same thread set up in its last few NVN calls, a weaker bound (another frame may reuse the stack), reported separately.
 * At exit (and every 20 s) the per-type bounds go to stderr: "[napple-gfx] objtrace <type>: host <n> min gap <g> (<neighbour>) ... gaps: <g> x<count> ...". */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "obj.h"

int g_objtrace_on;
__attribute__((constructor)) static void ot_env(void) { const char *e = getenv("NAPPLE_GFX_OBJTRACE"); g_objtrace_on = e && e[0] && e[0] != '0'; }

static const struct { const char *name; size_t host, reserved; } k_info[OT_N] = {
    [OT_DEVICE] = {"NVNdevice", sizeof(dev_t_), sizeof(NVNdevice)}, [OT_QUEUE] = {"NVNqueue", sizeof(queue_t), sizeof(NVNqueue)}, [OT_SYNC] = {"NVNsync", sizeof(sync_t), sizeof(NVNsync)},
    [OT_EVENT] = {"NVNevent", sizeof(ev_t), sizeof(NVNevent)}, [OT_POOL] = {"NVNmemoryPool", sizeof(pool_t), sizeof(NVNmemoryPool)}, [OT_BUFFER] = {"NVNbuffer", sizeof(buf_t), sizeof(NVNbuffer)},
    [OT_TEXTURE] = {"NVNtexture", sizeof(tex_t), sizeof(NVNtexture)}, [OT_SAMPLER] = {"NVNsampler", sizeof(samp_t), sizeof(NVNsampler)}, [OT_TEXPOOL] = {"NVNtexturePool", sizeof(dpool_t), sizeof(NVNtexturePool)},
    [OT_SAMPPOOL] = {"NVNsamplerPool", sizeof(dpool_t), sizeof(NVNsamplerPool)}, [OT_PROGRAM] = {"NVNprogram", sizeof(prog_t), sizeof(NVNprogram)}, [OT_WINDOW] = {"NVNwindow", sizeof(win_t), sizeof(NVNwindow)},
    [OT_CMDBUF] = {"NVNcommandBuffer", sizeof(cmd_t), sizeof(NVNcommandBuffer)},
    [OT_DEVICE_B] = {"NVNdeviceBuilder", sizeof(dev_builder_t), sizeof(NVNdeviceBuilder)}, [OT_QUEUE_B] = {"NVNqueueBuilder", sizeof(queue_builder_t), sizeof(NVNqueueBuilder)},
    [OT_POOL_B] = {"NVNmemoryPoolBuilder", sizeof(pool_builder_t), sizeof(NVNmemoryPoolBuilder)}, [OT_BUFFER_B] = {"NVNbufferBuilder", sizeof(buf_builder_t), sizeof(NVNbufferBuilder)},
    [OT_TEXTURE_B] = {"NVNtextureBuilder", sizeof(tex_builder_t), sizeof(NVNtextureBuilder)}, [OT_SAMPLER_B] = {"NVNsamplerBuilder", sizeof(samp_builder_t), sizeof(NVNsamplerBuilder)},
    [OT_WINDOW_B] = {"NVNwindowBuilder", sizeof(win_builder_t), sizeof(NVNwindowBuilder)}, [OT_EVENT_B] = {"NVNeventBuilder", sizeof(ev_builder_t), sizeof(NVNeventBuilder)},
    [OT_VIEW] = {"NVNtextureView", sizeof(view_t), sizeof(NVNtextureView)}, [OT_BLEND] = {"NVNblendState", sizeof(blend_st), sizeof(NVNblendState)}, [OT_COLOR] = {"NVNcolorState", sizeof(color_st), sizeof(NVNcolorState)},
    [OT_CHMASK] = {"NVNchannelMaskState", sizeof(chmask_st), sizeof(NVNchannelMaskState)}, [OT_MS] = {"NVNmultisampleState", sizeof(ms_st), sizeof(NVNmultisampleState)}, [OT_POLY] = {"NVNpolygonState", sizeof(poly_st), sizeof(NVNpolygonState)},
    [OT_DS] = {"NVNdepthStencilState", sizeof(ds_st), sizeof(NVNdepthStencilState)}, [OT_VA] = {"NVNvertexAttribState", sizeof(va_st), sizeof(NVNvertexAttribState)}, [OT_VS] = {"NVNvertexStreamState", sizeof(vs_st), sizeof(NVNvertexStreamState)},
};

#define NGAP 12          /* distinct gap values kept per type (the smallest ones) */
#define GAP_MAX 0x4000   /* gaps above this say nothing about object sizes */
static struct { uint64_t objects, stack, reinit; uint32_t gap[NGAP], cnt[NGAP]; uint8_t with[NGAP]; } g_st[OT_N];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void add_gap(int k, uint64_t gap, int with) {
    if (!gap || gap > GAP_MAX) return;
    uint32_t g = (uint32_t)gap;
    int slot = -1;
    for (int i = 0; i < NGAP; i++) {
        if (g_st[k].cnt[i] && g_st[k].gap[i] == g) { g_st[k].cnt[i]++; return; }
        if (!g_st[k].cnt[i] && slot < 0) slot = i;
    }
    if (slot < 0) {                       /* full: replace the largest gap if this one is smaller */
        int big = 0;
        for (int i = 1; i < NGAP; i++) if (g_st[k].gap[i] > g_st[k].gap[big]) big = i;
        if (g_st[k].gap[big] <= g) return;
        slot = big;
    }
    g_st[k].gap[slot] = g; g_st[k].cnt[slot] = 1; g_st[k].with[slot] = (uint8_t)with;
}

/* live objects with a lifetime, ordered by address */
typedef struct { uintptr_t a; int k; uint32_t magic, reinit; } live_t;   /* magic: the object's first word once initialized (0 = not seen yet); reinit: set up again without Finalize */
static live_t *g_live; static size_t g_nlive, g_caplive;
static size_t live_find(uintptr_t a) {      /* first index with .a >= a */
    size_t lo = 0, hi = g_nlive;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (g_live[m].a < a) lo = m + 1; else hi = m; }
    return lo;
}

/* A live-set entry whose first word changed since it was initialized is memory the guest freed without Finalize: dropped. */
static int still_alive(size_t i) {
    if (g_live[i].reinit) return 1;                       /* kept (it marks the address) but never used as a neighbour: see usable() */
    uint32_t now = *(const volatile uint32_t *)g_live[i].a;
    if (!g_live[i].magic) { g_live[i].magic = now; return now != 0; }
    return now == g_live[i].magic;
}
static int usable(size_t i) { return !g_live[i].reinit; }
static void close_report(int k, uintptr_t a, int nk, uintptr_t n) {   /* a neighbour inside nvn.h's reserved size: this title's object is smaller; logged per (type, gap) */
    static uint8_t seen[OT_N][64];
    uint64_t gap = n - a;
    if (gap >= k_info[k].reserved) return;
    unsigned slot = (unsigned)(gap / 8) & 63;
    if (seen[k][slot]) return;
    seen[k][slot] = 1;
    fprintf(stderr, "[napple-gfx] objtrace close: %s at %#lx has a %s at +%#llx (reserved %#zx, host layout %#zx): first words %08x %08x\n", k_info[k].name,
            (unsigned long)a, k_info[nk].name, (unsigned long long)gap, k_info[k].reserved, k_info[k].host, *(const uint32_t *)a, *(const uint32_t *)n);
}

/* same-thread recent objects, for objects without a lifetime */
#define RECENT 12
static __thread struct { uintptr_t a; int k; } t_recent[RECENT];
static __thread unsigned t_nrecent;

static void dump(void) {
    pthread_mutex_lock(&g_mu);
    for (int k = 0; k < OT_N; k++) {
        if (!g_st[k].objects) continue;
        int ord[NGAP], n = 0;
        for (int i = 0; i < NGAP; i++) if (g_st[k].cnt[i]) ord[n++] = i;
        for (int i = 1; i < n; i++) for (int j = i; j > 0 && g_st[k].gap[ord[j]] < g_st[k].gap[ord[j - 1]]; j--) { int t = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = t; }
        char buf[1024]; int o = 0;
        for (int i = 0; i < n && o < (int)sizeof buf - 64; i++)
            o += snprintf(buf + o, sizeof buf - (size_t)o, " %#x x%u (%s)", g_st[k].gap[ord[i]], g_st[k].cnt[ord[i]], k_info[g_st[k].with[ord[i]]].name + 3);
        fprintf(stderr, "[napple-gfx] objtrace %s%s: reserved %#zx host %#zx, %llu set up (%llu on the calling stack, %llu set up again without Finalize); gaps:%s\n", k_info[k].name,
                k >= OT_HEAP_N ? " (no lifetime: weak)" : "", k_info[k].reserved, k_info[k].host, (unsigned long long)g_st[k].objects, (unsigned long long)g_st[k].stack,
                (unsigned long long)g_st[k].reinit, n ? buf : " none");
    }
    pthread_mutex_unlock(&g_mu);
}

void gfx_objtrace(int k, const void *p, int alive) {
    static _Atomic int reg;
    static _Atomic uint64_t last_dump;
    if (!atomic_exchange(&reg, 1)) atexit(dump);
    uintptr_t a = (uintptr_t)p;
    if (!a || k < 0 || k >= OT_N) return;
    /* 64-bit titles run on the guest's stack: an object just above this frame is a local of the caller chain (a temporary, never finalized) */
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    int on_stack = a > fp && a - fp < 0x20000;
    pthread_mutex_lock(&g_mu);
    if (k < OT_HEAP_N && on_stack) {
        if (alive) { g_st[k].objects++; g_st[k].stack++; }
    } else if (k < OT_HEAP_N) {
        size_t i = live_find(a);
        int here = i < g_nlive && g_live[i].a == a;
        if (!alive) {
            if (here) { memmove(g_live + i, g_live + i + 1, (g_nlive - i - 1) * sizeof *g_live); g_nlive--; }
        } else {
            g_st[k].objects++;
            if (here) { g_st[k].reinit += !g_live[i].reinit; g_live[i].reinit += g_live[i].k == k; g_live[i].k = k; g_live[i].magic = 0; }
            else {
                if (g_nlive == g_caplive) { g_caplive = g_caplive ? 2 * g_caplive : 4096; g_live = realloc(g_live, g_caplive * sizeof *g_live); }
                memmove(g_live + i + 1, g_live + i, (g_nlive - i) * sizeof *g_live);
                g_live[i] = (live_t){a, k, 0, 0};
                g_nlive++;
            }
            while (i + 1 < g_nlive && !still_alive(i + 1)) { memmove(g_live + i + 1, g_live + i + 2, (g_nlive - i - 2) * sizeof *g_live); g_nlive--; }
            while (i > 0 && !still_alive(i - 1)) { memmove(g_live + i - 1, g_live + i, (g_nlive - i) * sizeof *g_live); g_nlive--; i--; }
            if (usable(i)) {
                if (i + 1 < g_nlive && usable(i + 1)) { add_gap(k, g_live[i + 1].a - a, g_live[i + 1].k); close_report(k, a, g_live[i + 1].k, g_live[i + 1].a); }
                if (i > 0 && usable(i - 1)) { add_gap(g_live[i - 1].k, a - g_live[i - 1].a, k); close_report(g_live[i - 1].k, g_live[i - 1].a, k, a); }
            }
        }
    } else if (alive) {
        g_st[k].objects++;
        g_st[k].stack += on_stack;
        for (unsigned j = 0; j < RECENT && j < t_nrecent; j++) {
            uintptr_t q = t_recent[j].a;
            int qk = t_recent[j].k;
            if (q > a) add_gap(k, q - a, qk);
            else if (q < a && qk >= OT_HEAP_N) add_gap(qk, a - q, k);
        }
    }
    if (alive) { t_recent[t_nrecent % RECENT].a = a; t_recent[t_nrecent % RECENT].k = k; t_nrecent++; }
    pthread_mutex_unlock(&g_mu);
    uint64_t now = gfx_now_ns(), last = atomic_load(&last_dump);
    if (now - last > 20000000000ull && atomic_compare_exchange_strong(&last_dump, &last, now) && last) dump();
}
