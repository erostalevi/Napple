/* api_cmd.c - NVN command buffers. Commands are recorded into host memory as compact records; EndRecording publishes an immutable,
 * reference-counted list that submissions and CallCommands refer to by handle. Nothing here touches Metal. */
#include <compression.h>
#include <dlfcn.h>
#include <os/lock.h>
#include <pthread.h>
#include <sched.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include "gfx_mtl.h"
#include "hostobj.h"

void gfx_api_link_cmd(void) {}

#define CB_MAGIC MAGIC('C', 'M', 'D', '0')
#define TEX_MAGIC MAGIC('T', 'E', 'X', '0')
#define PROG_MAGIC MAGIC('P', 'R', 'G', '0')

/* The host part of a guest command buffer object. The guest object only holds a slot number and a generation (cmd_t.hid), never a pointer, and host parts
 * are never freed: a finalized one keeps its slot and is reused by the next Initialize. A guest object that was copied, finalized, overwritten or left on a
 * dead stack therefore resolves to "no host part" instead of to freed memory. ACNH starts used one stack-allocated command buffer from two threads at once
 * (two threads on two cores recorded the same object's slot; see the 2026-10-28 b8 change note): every change of the recording state is made under the host part's
 * lock, buffers that grow are freed only after a grace period (a writer of the other thread may still hold a pointer into them), and a command that has
 * nowhere to go is written to a per-thread sink as large as the command. */
typedef struct cmdbuf_host {
    os_unfair_lock lk;                        /* recording state: buf/len/cap, children, rec_* (getters and setters of plain values do not take it) */
    _Atomic uint32_t gen;                     /* generation of the guest object using this part; 0 while the part is free */
    uint32_t slot;
    uint8_t *buf; size_t len, cap;            /* scratch for the recording in progress */
    cmdrec **children; uint32_t nchild, capchild;
    uintptr_t rec_lo;                         /* where the recording in progress started in command memory */
    uintptr_t cmd_base, ctl_base;
    size_t cmd_size, ctl_size, used_est;
    PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb; void *cb_data;
    ptrdiff_t row_stride, img_stride;
    uint64_t rec_tid;                         /* thread that called BeginRecording (diagnostics: a recording used from two threads) */
    const void *owner;                        /* the guest cmd_t this host part was made for: a copied or moved guest object must not share it */
} cmdbuf_host;
/* Slot table: fixed chunks, published once and never moved or freed, so a lookup needs no lock. */
#define CBH_CHUNK 4096u
#define CBH_CHUNKS 1024u
static _Atomic(cmdbuf_host *) *_Atomic g_cbh_tab[CBH_CHUNKS];
static _Atomic uint32_t g_cbh_n, g_cbh_gen;
static uint32_t *g_cbh_free, g_cbh_nfree, g_cbh_capfree;
static pthread_mutex_t g_cbh_mu = PTHREAD_MUTEX_INITIALIZER;
static cmdbuf_host *cbh_slot(uint32_t slot) {
    if (slot >= atomic_load_explicit(&g_cbh_n, memory_order_acquire)) return NULL;
    _Atomic(cmdbuf_host *) *ch = atomic_load_explicit(&g_cbh_tab[slot / CBH_CHUNK], memory_order_acquire);
    return ch ? atomic_load_explicit(&ch[slot % CBH_CHUNK], memory_order_acquire) : NULL;
}
/* The host part of a guest command buffer object, or NULL when the object was never initialised, was finalized, is a byte copy of another one (the guest
 * may relocate or copy the object; two objects sharing one host part would double free it) or holds garbage. The part stays valid memory for good; callers
 * that change it take its lock and check cbh_live again. */
static inline int cbh_live(const cmdbuf_host *h, const cmd_t *o, uint32_t gen) { return gen && atomic_load_explicit(&h->gen, memory_order_acquire) == gen && h->owner == o; }
static cmdbuf_host *cb_host_of(const cmd_t *o) {
    if (!o) return NULL;
    uint64_t id = o->hid;
    cmdbuf_host *h = (uint32_t)id ? cbh_slot((uint32_t)id - 1) : NULL;
    return h && cbh_live(h, o, (uint32_t)(id >> 32)) ? h : NULL;
}
/* Locks the host part of o; NULL (nothing locked) when it has none. */
static cmdbuf_host *cbh_lock(const cmd_t *o) {
    cmdbuf_host *h = cb_host_of(o);
    if (!h) return NULL;
    os_unfair_lock_lock(&h->lk);
    if (cbh_live(h, o, (uint32_t)(o->hid >> 32))) return h;
    os_unfair_lock_unlock(&h->lk);                                  /* finalized by another thread meanwhile */
    return NULL;
}
static void cbh_unlock(cmdbuf_host *h) { os_unfair_lock_unlock(&h->lk); }

/* Deferred frees: a buffer replaced while another thread may still be writing a command into it is freed GRAVE_NS later. */
#define GRAVE_NS 2000000000ull
typedef struct grave { struct grave *next; void *p; uint64_t t; } grave;
static grave *g_grave_head, **g_grave_tail = &g_grave_head;
static pthread_mutex_t g_grave_mu = PTHREAD_MUTEX_INITIALIZER;
static void grave_put(void *p) {
    grave *g = malloc(sizeof *g), *dead = NULL;
    uint64_t now = gfx_now_ns();
    pthread_mutex_lock(&g_grave_mu);
    if (g) { *g = (grave){NULL, p, now}; *g_grave_tail = g; g_grave_tail = &g->next; }
    while (g_grave_head && now - g_grave_head->t >= GRAVE_NS) {        /* oldest first: collect the expired ones */
        grave *x = g_grave_head;
        g_grave_head = x->next;
        if (!g_grave_head) g_grave_tail = &g_grave_head;
        x->next = dead; dead = x;
    }
    pthread_mutex_unlock(&g_grave_mu);
    if (!g) gfx_die("out of memory retiring a command buffer");
    while (dead) { grave *x = dead; dead = x->next; free(x->p); free(x); }
}

/* Per-thread sink for commands that are not recorded (no host part, or outside Begin/End): as large as the largest such command, zeroed. */
static _Thread_local uint8_t *t_sink;
static _Thread_local size_t t_sink_cap;
static void *cmd_sink(uint32_t payload) {
    size_t need = payload + 16u;
    if (need > t_sink_cap) {
        free(t_sink);
        t_sink_cap = need < 4096 ? 4096 : need * 2;
        t_sink = malloc(t_sink_cap);
        if (!t_sink) gfx_die("out of memory for a command sink");
    }
    memset(t_sink, 0, need);
    return t_sink;
}

static _Atomic uint64_t g_rec_live;
static _Atomic uint64_t g_rec_bytes;      /* bytes held by live recordings (headers + raw or packed command data) */
static _Atomic uint64_t g_rec_packed;     /* recordings currently held compressed */
static _Atomic uint64_t g_rel_why[5];     /* 0 hard cap, 1 command memory overwritten, 2 handle finalized, 3 pool freed, 4 unused */

static int g_cmdtrace = -1;                 /* NAPPLE_GFX_CMDTRACE=1: log recording lifetimes (command handle debugging) */
#define CMDTRACE(...) do { if (g_cmdtrace < 0) g_cmdtrace = getenv("NAPPLE_GFX_CMDTRACE") != NULL; if (g_cmdtrace) fprintf(stderr, "[cmdtrace] " __VA_ARGS__); } while (0)      /* recordings released by: grave cap, chunk reuse, dead, live cap, finalize (diagnostics) */

/* NAPPLE_GFX_CBTRACE=1: command buffer lifetime events (Initialize/Finalize/Begin/End and calls on objects without a host part) go to a ring buffer; the
 * first anomalies dump every event of the object involved (diagnostics for the ACNH start flake). */
typedef struct { uint64_t t; uint64_t tid; const void *o; uint64_t h; uintptr_t lr, fp, stk_hi, stk_lo, tsd; uint64_t gtid; int gcore; uint32_t ev, magic, rec; uintptr_t bt[6]; } cbev;
static uint64_t (*g_rt_tid)(void); static int (*g_rt_core)(void);   /* the runtime's guest thread id and core, when it is linked in (dlsym) */
enum { CBEV_INIT, CBEV_FIN, CBEV_BEGIN, CBEV_END, CBEV_NULL, CBEV_REC_NULL, CBEV_ANOM };
static const char *const cbev_name[] = {"init", "fin", "begin", "end", "null-host", "rec-null", "anomaly"};
#define CBRING 65536
static cbev *g_cbring; static _Atomic uint64_t g_cbpos; static int g_cbtrace = -1; static _Atomic int g_cbdumps;
static void cbtrace(uint32_t ev, const cmd_t *o, uintptr_t lr) {
    if (g_cbtrace < 0) {
        g_cbtrace = getenv("NAPPLE_GFX_CBTRACE") != NULL;
        if (g_cbtrace) { g_cbring = calloc(CBRING, sizeof *g_cbring); g_rt_tid = (uint64_t (*)(void))dlsym(RTLD_DEFAULT, "threads_current_id"); g_rt_core = (int (*)(void))dlsym(RTLD_DEFAULT, "threads_current_core"); }
    }
    if (!g_cbtrace || !g_cbring) return;
    uint64_t tid; pthread_threadid_np(NULL, &tid);
    cbev *e = &g_cbring[atomic_fetch_add(&g_cbpos, 1) % CBRING];
    uintptr_t hi = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
    uintptr_t tp; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
    *e = (cbev){gfx_now_ns(), tid, o, o ? o->hid : 0, lr, (uintptr_t)__builtin_frame_address(0), hi, hi - pthread_get_stacksize_np(pthread_self()), tp, g_rt_tid ? g_rt_tid() : 0, g_rt_core ? g_rt_core() : -9, ev, o ? o->magic : 0, o ? o->recording : 0, {0}};
    void **fp = __builtin_frame_address(0);
    for (int i = 0; i < 6 && fp && !((uintptr_t)fp & 7); i++, fp = (void **)fp[0] > fp ? (void **)fp[0] : NULL) e->bt[i] = (uintptr_t)fp[1];
}
static void cbtrace_dump(const char *why, const cmd_t *o, uintptr_t lr) {
    cbtrace(CBEV_ANOM, o, lr);
    if (!g_cbtrace || !g_cbring || atomic_fetch_add(&g_cbdumps, 1) >= 8) return;
    uint64_t end = atomic_load(&g_cbpos), beg = end > CBRING ? end - CBRING : 0;
    Dl_info di = {0};
    dladdr((void *)lr, &di);
    fprintf(stderr, "[cbtrace] %s: cb %p (h %#llx magic %#x rec %u) lr %#lx (%s+%#lx in %s); its events:\n", why, (const void *)o, o ? (unsigned long long)o->hid : 0ull, o ? o->magic : 0, o ? o->recording : 0, (unsigned long)lr,
            di.dli_sname ? di.dli_sname : "?", (unsigned long)(lr - (uintptr_t)di.dli_saddr), di.dli_fname ? di.dli_fname : "?");
    int n = 0;
    for (void **fp = __builtin_frame_address(0); fp && n < 12 && !((uintptr_t)fp & 7); fp = (void **)fp[0] > fp ? (void **)fp[0] : NULL, n++) {
        Dl_info fi = {0}; uintptr_t r = (uintptr_t)fp[1];
        if (!r) break;
        dladdr((void *)r, &fi);
        fprintf(stderr, "[cbtrace]   frame fp %p lr %#lx %s+%#lx\n", (void *)fp, (unsigned long)r, fi.dli_sname ? fi.dli_sname : "?", (unsigned long)(r - (uintptr_t)fi.dli_saddr));
    }
    if (o && (uint32_t)o->hid) { cmdbuf_host *hh = cbh_slot((uint32_t)o->hid - 1); fprintf(stderr, "[cbtrace]   slot %u of %u: host %p gen %u owner %p\n", (uint32_t)o->hid - 1, atomic_load(&g_cbh_n), (void *)hh, hh ? atomic_load(&hh->gen) : 0, hh ? hh->owner : NULL); }
    uint64_t seen[16]; int nseen = 0;
    for (uint64_t i = beg; i < end; i++) {
        const cbev *e = &g_cbring[i % CBRING];
        if (e->o != o && !(o && e->h && e->h == o->hid)) continue;
        fprintf(stderr, "[cbtrace]   %.6f tid %llu %-9s cb %p h %#llx magic %#x rec %u lr %#lx fp %#lx pthread stack [%#lx,%#lx) tsd %#lx guest thread %#llx core %d\n", e->t / 1e9, (unsigned long long)e->tid, cbev_name[e->ev], e->o, (unsigned long long)e->h, e->magic, e->rec, (unsigned long)e->lr,
                (unsigned long)e->fp, (unsigned long)e->stk_lo, (unsigned long)e->stk_hi, (unsigned long)e->tsd, (unsigned long long)e->gtid, e->gcore);
        int first = 1;                                              /* backtrace: the first event of each thread, and every anomaly */
        for (int k = 0; k < nseen; k++) if (seen[k] == e->tid) first = 0;
        if (first && nseen < 16) seen[nseen++] = e->tid;
        if (first || e->ev == CBEV_ANOM) {
            fprintf(stderr, "[cbtrace]     bt");
            for (int k = 0; k < 6 && e->bt[k]; k++) fprintf(stderr, " %#lx", (unsigned long)e->bt[k]);
            fprintf(stderr, "\n");
        }
    }
}
#define LR() ((uintptr_t)__builtin_return_address(0))

/* ---------------------------------------------------------------------------------------------------------------
 * Recordings
 * --------------------------------------------------------------------------------------------------------------- */
void cmdrec_retain(cmdrec *r) { atomic_fetch_add(&r->refs, 1); }
/* Handle lookups retain under g_acq_mu; the last release invalidates the handle and then passes through the same lock before freeing, so a lookup can never
 * hold a recording that is being freed (a thread re-recording a list at the same command memory used to free it between another thread's lookup and retain). */
static pthread_mutex_t g_acq_mu = PTHREAD_MUTEX_INITIALIZER;
void cmdrec_release(cmdrec *r) {
    if (atomic_fetch_sub(&r->refs, 1) != 1) return;
    CMDTRACE("free %#llx\n", (unsigned long long)r->handle);
    for (uint32_t i = 0; i < r->nchildren; i++) cmdrec_release(r->children[i]);
    free(r->children);
    htab_free(r->handle);
    pthread_mutex_lock(&g_acq_mu); pthread_mutex_unlock(&g_acq_mu);   /* lookups that already found r have retained it or failed on refs == 0 */
    r->magic = 0;
    atomic_fetch_sub(&g_rec_live, 1);
    if (r->z) atomic_fetch_sub(&g_rec_packed, 1);
    atomic_fetch_sub(&g_rec_bytes, sizeof *r + (r->z ? r->zlen : r->len));
    free(r->data); free(r->z);
    free(r);
}
cmdrec *cmdrec_acquire(uint64_t h) {          /* lookup + retain; NULL when stale */
    pthread_mutex_lock(&g_acq_mu);
    cmdrec *r = (cmdrec *)htab_get(h);
    if (r && r->magic == CMDREC_MAGIC) {
        int n = atomic_load(&r->refs);
        while (n > 0 && !atomic_compare_exchange_weak(&r->refs, &n, n + 1)) {}
        if (n <= 0) r = NULL;
    } else r = NULL;
    pthread_mutex_unlock(&g_acq_mu);
    return r;
}
cmdrec *cmdrec_from_handle(uint64_t h) {
    cmdrec *r = (cmdrec *)htab_get(h);
    return r && r->magic == CMDREC_MAGIC ? r : NULL;
}

/* Command data access. Recordings that were not touched for a while are held LZ4-packed (state blocks repeat, so they shrink several times); the first use
 * after that unpacks them again. A recording is touched (last_use) on every access, and the packer only takes ones idle for PACK_IDLE_S seconds, far longer than
 * any replay, so the pointer returned here stays valid while the caller walks the list. */
static uint32_t g_pack_idle = 12;
#define PACK_IDLE_S g_pack_idle
static void rec_lock(cmdrec *r) { while (atomic_exchange_explicit(&r->lk, 1, memory_order_acquire)) sched_yield(); }
static void rec_unlock(cmdrec *r) { atomic_store_explicit(&r->lk, 0, memory_order_release); }
const uint8_t *cmdrec_data(const cmdrec *cr) {
    cmdrec *r = (cmdrec *)(uintptr_t)cr;
    atomic_store_explicit(&r->last_use, cmdrec_now(), memory_order_relaxed);
    for (;;) {
    uint8_t *dp = __atomic_load_n(&r->data, __ATOMIC_ACQUIRE);
    if (dp) return dp;                                            /* packed state only changes under the lock; a non-NULL pointer is stable while hot */
    rec_lock(r);
    if (!r->data) {
        uint8_t *d = malloc(r->len ? r->len : 1);
        if (!d || compression_decode_buffer(d, r->len, r->z, r->zlen, NULL, COMPRESSION_LZ4) != r->len) gfx_die("command list unpack failed");
        free(r->z); r->z = NULL;
        atomic_fetch_sub(&g_rec_packed, 1);
        atomic_fetch_sub(&g_rec_bytes, r->zlen);
        atomic_fetch_add(&g_rec_bytes, r->len);
        __atomic_store_n(&r->data, d, __ATOMIC_RELEASE);
    }
    rec_unlock(r);
    }
}
static void rec_pack(cmdrec *r) {
    if (r->len < 512 || !r->data) return;
    uint8_t *z = malloc(r->len);
    if (!z) return;
    size_t n = compression_encode_buffer(z, r->len, r->data, r->len, NULL, COMPRESSION_LZ4);
    if (!n || n > r->len * 3 / 4) { free(z); return; }
    z = realloc(z, n);
    rec_lock(r);
    if (r->data && cmdrec_now() - atomic_load(&r->last_use) >= PACK_IDLE_S) {
        uint8_t *d = r->data;
        r->z = z; r->zlen = (uint32_t)n;
        __atomic_store_n(&r->data, NULL, __ATOMIC_RELEASE);
        atomic_fetch_add(&g_rec_packed, 1);
        atomic_fetch_sub(&g_rec_bytes, r->len);
        atomic_fetch_add(&g_rec_bytes, n);
        free(d);
    } else free(z);
    rec_unlock(r);
}

/* ---------------------------------------------------------------------------------------------------------------
 * Lifetime: a recording is valid as long as the command memory it was recorded into is intact
 * ---------------------------------------------------------------------------------------------------------------
 * On hardware a command handle is the address of the recorded commands inside the game's command memory, so it stays valid until that memory is freed (pool
 * finalized), overwritten (another list recorded at the same address) or the game finalizes the handle. Nothing else ends it: not the command buffer that
 * recorded it (SMO finalizes per-model scratch command buffers and replays their lists for minutes), and not idle time or a memory budget (both made SMO
 * call lists we had dropped: the model then drew without its setup, "exploded" geometry). The registry below owns one reference to every live recording.
 * Memory stays bounded by what the game itself allocated (every list lives in command memory the game owns); LZ4 packing of idle lists keeps the host cost low.
 * NAPPLE_REC_BUDGET_MB (default: a quarter of the physical memory) is only a safety ceiling for a runaway game: past it the least recently used are dropped with
 * a warning, which is a bug to investigate, never normal operation. */
static pthread_mutex_t g_reg_mu = PTHREAD_MUTEX_INITIALIZER;
static cmdrec **g_reg; static uint32_t g_nreg, g_capreg;
static cmdrec **g_hash; static uint32_t g_hmask;      /* by command memory address (lo): the "same address" overwrite test */
static uint64_t rec_ceiling(void) {
    static uint64_t b;
    if (!b) {
        const char *e = getenv("NAPPLE_REC_BUDGET_MB");
        uint64_t mem = 0; size_t sz = sizeof mem;
        if (sysctlbyname("hw.memsize", &mem, &sz, NULL, 0)) mem = 8ull << 30;
        b = e && atoll(e) > 0 ? (uint64_t)atoll(e) << 20 : mem / 4;
    }
    return b;
}
uint32_t cmdrec_now(void) { return (uint32_t)(gfx_now_ns() / 1000000000ull); }
static uint32_t hslot(uintptr_t lo) { return (uint32_t)((lo >> 3) * 0x9E3779B1u) & g_hmask; }
static void reg_hash_rebuild(void) {
    uint32_t n = g_hmask ? (g_hmask + 1) * 2 : 4096;
    free(g_hash); g_hash = calloc(n, sizeof *g_hash); g_hmask = n - 1;
    for (uint32_t i = 0; i < g_nreg; i++) { uint32_t h = hslot(g_reg[i]->lo); g_reg[i]->hnext = g_hash[h]; g_hash[h] = g_reg[i]; }
}
static void reg_add(cmdrec *r) {                                  /* g_reg_mu held */
    if (g_nreg == g_capreg) { g_capreg = g_capreg ? g_capreg * 2 : 4096; g_reg = realloc(g_reg, g_capreg * sizeof *g_reg); }
    r->reg_idx = g_nreg; g_reg[g_nreg++] = r;
    if (g_nreg > (g_hmask + 1) / 2) reg_hash_rebuild();
    else { uint32_t h = hslot(r->lo); r->hnext = g_hash[h]; g_hash[h] = r; }
}
static void reg_remove(cmdrec *r, int why) {                      /* g_reg_mu held; the registry reference is dropped by the caller after unlocking */
    cmdrec **pp = &g_hash[hslot(r->lo)];
    while (*pp && *pp != r) pp = &(*pp)->hnext;
    if (*pp) *pp = r->hnext;
    g_reg[r->reg_idx] = g_reg[--g_nreg]; g_reg[r->reg_idx]->reg_idx = r->reg_idx;
    r->reg_idx = ~0u;
    htab_free(r->handle);                                         /* the handle is invalid from now on, even if a calling list still holds the recording */
    atomic_fetch_add(&g_rel_why[why], 1);
}
/* drop every recording matching pred, collecting them for release outside the lock */
typedef int (*rec_pred)(const cmdrec *, uintptr_t, uintptr_t);
static void reg_drop_if(rec_pred pred, uintptr_t a, uintptr_t b, int why) {
    cmdrec **rel = NULL; uint32_t nrel = 0, caprel = 0;
    pthread_mutex_lock(&g_reg_mu);
    for (uint32_t i = 0; i < g_nreg;) {
        cmdrec *r = g_reg[i];
        if (!pred(r, a, b)) { i++; continue; }
        if (nrel == caprel) { caprel = caprel ? caprel * 2 : 64; rel = realloc(rel, caprel * sizeof *rel); }
        rel[nrel++] = r;
        reg_remove(r, why);                                       /* moves the last entry into slot i: do not advance */
    }
    pthread_mutex_unlock(&g_reg_mu);
    for (uint32_t i = 0; i < nrel; i++) { CMDTRACE("drop %#llx why %d\n", (unsigned long long)rel[i]->handle, why); cmdrec_release(rel[i]); }
    free(rel);
}
static int pred_in_range(const cmdrec *r, uintptr_t lo, uintptr_t hi) { return r->lo < hi && r->hi > lo; }

/* Called by nvnMemoryPoolFinalize: lists recorded into this pool's memory are gone. */
void gfx_cmdrec_pool_freed(const void *mem, size_t size) {
    if (!mem || !size) return;
    reg_drop_if(pred_in_range, (uintptr_t)mem, (uintptr_t)mem + size, 3);
}

/* Registers a finished recording; supersedes any list recorded at the same command memory address. */
static void reg_publish(cmdrec *r) {
    cmdrec *old = NULL, **rel = NULL; uint32_t nrel = 0;
    pthread_mutex_lock(&g_reg_mu);
    if (g_hash)
        for (cmdrec *c = g_hash[hslot(r->lo)]; c; c = c->hnext)
            if (c->lo == r->lo) { old = c; break; }
    if (old) reg_remove(old, 1);
    reg_add(r);
    /* safety ceiling */
    while (atomic_load(&g_rec_bytes) > rec_ceiling() && g_nreg > 1) {
        uint32_t vic = 0, best = ~0u;
        for (uint32_t i = 0, step = g_nreg / 4096 + 1; i < g_nreg; i += step) { uint32_t u = atomic_load(&g_reg[i]->last_use); if (g_reg[i] != r && u < best) { best = u; vic = i; } }
        cmdrec *v = g_reg[vic];
        if (v == r) break;
        LOG_ONCE(NAPPLE_LOG_WARN, "command lists exceed the %llu MB safety ceiling (%u alive): releasing the least recently used", (unsigned long long)(rec_ceiling() >> 20), g_nreg);
        reg_remove(v, 0);
        rel = realloc(rel, (nrel + 1) * sizeof *rel); rel[nrel++] = v;
        if (nrel >= 256) break;
    }
    pthread_mutex_unlock(&g_reg_mu);
    if (old) cmdrec_release(old);
    for (uint32_t i = 0; i < nrel; i++) cmdrec_release(rel[i]);
    free(rel);
}
void gfx_cmdrec_unregister(cmdrec *r) {                           /* the game finalized the handle */
    int mine = 0;
    pthread_mutex_lock(&g_reg_mu);
    if (r->reg_idx != ~0u && r->reg_idx < g_nreg && g_reg[r->reg_idx] == r) { reg_remove(r, 2); mine = 1; }
    pthread_mutex_unlock(&g_reg_mu);
    if (mine) cmdrec_release(r);
}

/* Packer: a background thread walks the registry every few seconds and packs lists idle for PACK_IDLE_S. */
static void *packer_main(void *arg) {
    (void)arg;
    cmdrec *cand[256];
    for (;;) {
        usleep(2000000);
        static uint32_t cur;
        for (int rounds = 0; rounds < 64; rounds++) {
            uint32_t nc = 0, now = cmdrec_now();
            pthread_mutex_lock(&g_reg_mu);
            for (uint32_t k = 0; k < 4096 && g_nreg; k++, cur++) {
                if (cur >= g_nreg) cur = 0;
                cmdrec *r = g_reg[cur];
                if (r->data && r->len >= 512 && now - atomic_load(&r->last_use) >= PACK_IDLE_S && nc < 256) { cmdrec_retain(r); cand[nc++] = r; }
            }
            pthread_mutex_unlock(&g_reg_mu);
            for (uint32_t i = 0; i < nc; i++) { rec_pack(cand[i]); cmdrec_release(cand[i]); }
            if (!nc) break;
        }
    }
    return NULL;
}
static pthread_once_t g_packer_once = PTHREAD_ONCE_INIT;
static void packer_start(void) { pthread_t t; if (!getenv("NAPPLE_REC_NO_PACK") && pthread_create(&t, NULL, packer_main, NULL) == 0) pthread_detach(t); }
/* Test hooks: is the handle callable; pack every idle-enough list now (synchronously). */
int gfx_cmdrec_valid(uint64_t handle) { return cmdrec_from_handle(handle) != NULL; }
uint32_t gfx_cmdrec_pack_now(uint32_t idle_s) {
    uint32_t old = g_pack_idle, n = 0;
    g_pack_idle = idle_s;
    cmdrec **cand = NULL; uint32_t nc = 0;
    pthread_mutex_lock(&g_reg_mu);
    cand = malloc((g_nreg + 1) * sizeof *cand);
    for (uint32_t i = 0; i < g_nreg; i++) if (g_reg[i]->data) { cmdrec_retain(g_reg[i]); cand[nc++] = g_reg[i]; }
    pthread_mutex_unlock(&g_reg_mu);
    for (uint32_t i = 0; i < nc; i++) { rec_pack(cand[i]); if (!cand[i]->data) n++; cmdrec_release(cand[i]); }
    free(cand);
    g_pack_idle = old;
    return n;
}
uint64_t gfx_cmdrec_live(void) { return atomic_load(&g_rec_live); }
uint64_t gfx_cmdrec_bytes(void) { return atomic_load(&g_rec_bytes); }
uint64_t gfx_cmdrec_packed(void) { return atomic_load(&g_rec_packed); }

/* ---------------------------------------------------------------------------------------------------------------
 * Command buffer object
 * --------------------------------------------------------------------------------------------------------------- */
/* A command buffer that was never initialised or already finalized has no host part (h == NULL): the game may still call getters/setters on it (AC v0 crashed in
 * GetCommandMemoryUsed on a null buffer). They then act on a scratch host object, so the call is a harmless no-op instead of a fault. */
static cmdbuf_host g_null_host;
static cmdbuf_host *cbh_get(const void *c) {
    cmdbuf_host *h = cb_host_of((const cmd_t *)c);
    if (!h) { cbtrace_dump("call on a null or finalized command buffer", c, LR()); LOG_ONCE(NAPPLE_LOG_WARN, "command buffer call on a null or finalized command buffer ignored"); return &g_null_host; }
    return h;
}
#define CBH(c) cbh_get(c)

/* A free host part (from the free list, or a new slot). */
static cmdbuf_host *cbh_alloc(void) {
    cmdbuf_host *h = NULL;
    pthread_mutex_lock(&g_cbh_mu);
    if (g_cbh_nfree) h = cbh_slot(g_cbh_free[--g_cbh_nfree]);
    else {
        uint32_t slot = atomic_load(&g_cbh_n);
        _Atomic(cmdbuf_host *) *ch = slot < CBH_CHUNK * CBH_CHUNKS ? atomic_load(&g_cbh_tab[slot / CBH_CHUNK]) : NULL;
        if (slot < CBH_CHUNK * CBH_CHUNKS && !ch && (ch = calloc(CBH_CHUNK, sizeof *ch))) atomic_store_explicit(&g_cbh_tab[slot / CBH_CHUNK], ch, memory_order_release);
        if (ch && (h = calloc(1, sizeof *h)) && (h->buf = malloc(4096))) {
            h->lk = OS_UNFAIR_LOCK_INIT;
            h->slot = slot;
            h->cap = 4096;
            atomic_store_explicit(&ch[slot % CBH_CHUNK], h, memory_order_release);
            atomic_store_explicit(&g_cbh_n, slot + 1, memory_order_release);
        } else if (h) { free(h); h = NULL; }
    }
    pthread_mutex_unlock(&g_cbh_mu);
    return h;
}
/* Ends the guest object's use of h (locked; unlocked on return) and puts it on the free list. */
static void cbh_retire(cmdbuf_host *h) {
    /* recorded lists stay valid: they belong to the game's command memory, not to this object */
    for (uint32_t i = 0; i < h->nchild; i++) cmdrec_release(h->children[i]);
    h->nchild = 0;
    h->len = 0;
    atomic_store_explicit(&h->gen, 0, memory_order_release);
    h->owner = NULL;
    if (h->cap > (1u << 20)) {                                      /* do not keep a huge scratch on a free part */
        uint8_t *nb = malloc(4096);
        if (!nb) gfx_die("out of memory finalizing a command buffer");
        grave_put(h->buf);
        h->buf = nb; h->cap = 4096;
    }
    uint32_t slot = h->slot;
    cbh_unlock(h);
    pthread_mutex_lock(&g_cbh_mu);
    if (g_cbh_nfree == g_cbh_capfree) {
        uint32_t nc = g_cbh_capfree ? g_cbh_capfree * 2 : 256;
        uint32_t *nf = realloc(g_cbh_free, nc * sizeof *nf);
        if (!nf) gfx_die("out of memory finalizing a command buffer");
        g_cbh_free = nf; g_cbh_capfree = nc;
    }
    g_cbh_free[g_cbh_nfree++] = slot;
    pthread_mutex_unlock(&g_cbh_mu);
}

NVN(nvnCommandBufferInitialize, NVNboolean, NVNcommandBuffer *c, NVNdevice *d) {
    (void)d;
    cmd_t *o = (cmd_t *)c;
    cmdbuf_host *old = cbh_lock(o);
    if (old) cbh_retire(old);                                       /* initialised again without Finalize */
    memset(o, 0, sizeof *o);
    cmdbuf_host *h = cbh_alloc();
    if (!h) return 0;
    uint32_t gen;
    do gen = atomic_fetch_add(&g_cbh_gen, 1) + 1; while (!gen);
    os_unfair_lock_lock(&h->lk);
    h->len = 0; h->nchild = 0; h->rec_lo = 0;
    h->cmd_base = h->ctl_base = 0; h->cmd_size = h->ctl_size = h->used_est = 0;
    h->cb = NULL; h->cb_data = NULL;
    h->row_stride = h->img_stride = 0;
    h->rec_tid = 0;
    h->owner = o;
    atomic_store_explicit(&h->gen, gen, memory_order_release);
    cbh_unlock(h);
    o->magic = CB_MAGIC;
    o->hid = (uint64_t)gen << 32 | (h->slot + 1);
    o->inline_cur = ~0ull;      /* NVN 55 inline fast paths never find room: every call takes the API path */
    o->inline_end = 0;
    cbtrace(CBEV_INIT, o, LR());
    return 1;
}
NVN(nvnCommandBufferFinalize, void, NVNcommandBuffer *c) {
    cmd_t *o = (cmd_t *)c;
    cbtrace(CBEV_FIN, o, LR());
    cmdbuf_host *h = o->magic == CB_MAGIC ? cbh_lock(o) : NULL;
    if (h) {
        CMDTRACE("finalize cb %p\n", (void *)h);
        cbh_retire(h);
    }
    memset(o, 0, sizeof *o);
}
NVN(nvnCommandBufferSetDebugLabel, void, NVNcommandBuffer *c, const char *l) { (void)c; (void)l; }
NVN(nvnCommandBufferSetMemoryCallback, void, NVNcommandBuffer *c, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb) { CBH(c)->cb = cb; }
NVN(nvnCommandBufferSetMemoryCallbackData, void, NVNcommandBuffer *c, void *d) { CBH(c)->cb_data = d; }
NVN(nvnCommandBufferGetMemoryCallback, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC, const NVNcommandBuffer *c) { return CBH(c)->cb; }
NVN(nvnCommandBufferGetMemoryCallbackData, void *, const NVNcommandBuffer *c) { return CBH(c)->cb_data; }
/* Recordings live in host memory, so command memory never runs out and the out-of-memory callback is never due: enabling it changes nothing. */
NVN(nvnCommandBufferSetCommandMemoryCallbackEnabled, void, NVNcommandBuffer *c, uint32_t e) { (void)c; (void)e; }

/* Command and control memory belong to the game; recordings live in host memory, so the memory is only accounted (see the lifetime notes above). */
NVN(nvnCommandBufferAddCommandMemory, void, NVNcommandBuffer *c, const NVNmemoryPool *p, ptrdiff_t off, size_t size) {
    uintptr_t base = (uintptr_t)((const pool_t *)p)->mem + (uintptr_t)off;
    cmdbuf_host *h = cbh_lock((const cmd_t *)c);
    if (!h) { CBH(c); return; }                                     /* (logs the call on a null buffer) */
    CMDTRACE("addmem cb %p [%#lx,%#lx) prev [%#lx,+%#zx) used %#zx\n", (void *)h, (unsigned long)base, (unsigned long)(base + size), (unsigned long)h->cmd_base, h->cmd_size, h->used_est);
    h->cmd_base = base; h->cmd_size = size; h->used_est = 0;
    cbh_unlock(h);
}
NVN(nvnCommandBufferAddControlMemory, void, NVNcommandBuffer *c, void *m, size_t size) { cmdbuf_host *h = CBH(c); h->ctl_base = (uintptr_t)m; h->ctl_size = size; }
NVN(nvnCommandBufferGetCommandMemorySize, size_t, const NVNcommandBuffer *c) { return CBH(c)->cmd_size; }
NVN(nvnCommandBufferGetCommandMemoryUsed, size_t, const NVNcommandBuffer *c) { return CBH(c)->used_est; }
NVN(nvnCommandBufferGetCommandMemoryFree, size_t, const NVNcommandBuffer *c) { const cmdbuf_host *h = CBH(c); return h->cmd_size > h->used_est ? h->cmd_size - h->used_est : 0; }
NVN(nvnCommandBufferGetControlMemorySize, size_t, const NVNcommandBuffer *c) { return CBH(c)->ctl_size; }
/* Control memory is not consumed (recordings are host-side), so none is in use and all of it is free. */
NVN(nvnCommandBufferGetControlMemoryUsed, size_t, const NVNcommandBuffer *c) { (void)c; return 0; }
NVN(nvnCommandBufferGetControlMemoryFree, size_t, const NVNcommandBuffer *c) { return CBH(c)->ctl_size; }
NVN(nvnCommandBufferIsRecording, NVNboolean, const NVNcommandBuffer *c) { return c ? ((const cmd_t *)c)->recording : 0; }

/* ---------------------------------------------------------------------------------------------------------------
 * Record writer
 * --------------------------------------------------------------------------------------------------------------- */
/* Appends a record (h locked) and returns its zeroed payload. The caller fills the payload after the lock is dropped: a buffer that grows is therefore only
 * retired (freed GRAVE_NS later), and a payload that another thread's EndRecording copies before it is filled reads as zeros (an empty record). */
static void *rec_op_locked(cmdbuf_host *h, uint16_t op, uint32_t payload) {
    uint32_t size = (uint32_t)((sizeof(op_hdr) + payload + 7) & ~7u);
    if (h->len + size > h->cap) {
        size_t nc = h->cap * 2;
        while (nc < h->len + size) nc *= 2;
        uint8_t *nb = malloc(nc);
        if (!nb) gfx_die("out of memory recording commands");
        memcpy(nb, h->buf, h->len);
        grave_put(h->buf);
        h->buf = nb; h->cap = nc;
    }
    op_hdr *hd = (op_hdr *)(h->buf + h->len);
    hd->op = op; hd->flags = 0; hd->size = size;
    h->len += size;
    h->used_est += 16 + payload / 2;
    uint8_t *p = (uint8_t *)(hd + 1);
    memset(p, 0, size - sizeof *hd);
    return p;
}
static void *rec_op(NVNcommandBuffer *c, uint16_t op, uint32_t payload) {
    cmd_t *o = (cmd_t *)c;
    cmdbuf_host *h = cbh_lock(o);
    if (!h) { cbtrace_dump("command recorded on a null or finalized command buffer", o, LR()); LOG_ONCE(NAPPLE_LOG_WARN, "command recorded on a null or finalized command buffer ignored"); return cmd_sink(payload); }
    if (!o->recording) {
        cbh_unlock(h);
        cbtrace_dump("command recorded outside Begin/End", o, LR());
        LOG_ONCE(NAPPLE_LOG_WARN, "command recorded outside BeginRecording/EndRecording (op %u) ignored", op);
        return cmd_sink(payload);
    }
    { static int tr = -1; if (tr < 0) tr = getenv("NAPPLE_NVNTRACE") != NULL; if (tr) fprintf(stderr, "[nvntrace] cb %p op %u\n", (void *)c, op); }     /* record-order op stream (diagnostics) */
    void *p = rec_op_locked(h, op, payload);
    cbh_unlock(h);
    return p;
}
#define REC(c, T, op) ((T *)rec_op((c), (op), sizeof(T)))
#define REC_N(c, T, op, extra) ((T *)rec_op((c), (op), (uint32_t)(sizeof(T) + (extra))))


NVN(nvnCommandBufferBeginRecording, void, NVNcommandBuffer *c) {
    cmd_t *o = (cmd_t *)c;
    cbtrace(CBEV_BEGIN, o, LR());
    cmdbuf_host *h = cbh_lock(o);
    if (!h) { cbtrace_dump("BeginRecording on a null or finalized command buffer", o, LR()); LOG_ONCE(NAPPLE_LOG_WARN, "BeginRecording on a null or finalized command buffer ignored"); return; }
    uint64_t me = (uint64_t)(uintptr_t)pthread_self();
    if (o->recording) cbtrace_dump("BeginRecording while already recording", o, LR());
    if (o->recording) LOG_ONCE(NAPPLE_LOG_WARN, "BeginRecording while already recording (first begun by thread %#llx, now %#llx, cb %p)", (unsigned long long)h->rec_tid, (unsigned long long)me, (void *)c);
    h->rec_tid = me;
    h->len = 0;
    h->rec_lo = h->cmd_base + h->used_est;
    for (uint32_t i = 0; i < h->nchild; i++) cmdrec_release(h->children[i]);
    h->nchild = 0;
    o->recording = 1;
    cbh_unlock(h);
}

NVN(nvnCommandBufferEndRecording, NVNcommandHandle, NVNcommandBuffer *c) {
    cmd_t *o = (cmd_t *)c;
    cbtrace(CBEV_END, o, LR());
    cmdbuf_host *h = cbh_lock(o);
    if (!h) { cbtrace_dump("EndRecording on a null or finalized command buffer", o, LR()); LOG_ONCE(NAPPLE_LOG_WARN, "EndRecording on a null or finalized command buffer ignored"); return 0; }
    if (!o->recording) cbtrace_dump("EndRecording without BeginRecording", o, LR());
    if (h->rec_tid != (uint64_t)(uintptr_t)pthread_self()) cbtrace_dump("EndRecording from another thread", o, LR());
    if (!o->recording) { LOG_ONCE(NAPPLE_LOG_WARN, "EndRecording without BeginRecording (last begun by thread %#llx, now %#llx, cb %p)", (unsigned long long)h->rec_tid, (unsigned long long)(uintptr_t)pthread_self(), (void *)c); cbh_unlock(h); return 0; }
    if (h->rec_tid != (uint64_t)(uintptr_t)pthread_self()) LOG_ONCE(NAPPLE_LOG_WARN, "EndRecording from another thread than BeginRecording (%#llx vs %#llx, cb %p)", (unsigned long long)h->rec_tid, (unsigned long long)(uintptr_t)pthread_self(), (void *)c);
    rec_op_locked(h, OP_END, 0);
    o->recording = 0;
    cmdrec *r = calloc(1, sizeof *r);
    uint8_t *data = malloc(h->len ? h->len : 1);
    if (!r || !data) gfx_die("out of memory finishing a command list");
    atomic_init(&r->refs, 1);                        /* the registry's reference */
    r->magic = CMDREC_MAGIC;
    r->len = (uint32_t)h->len;
    r->nchildren = h->nchild;
    if (h->nchild) { r->children = malloc(h->nchild * sizeof *r->children); memcpy(r->children, h->children, h->nchild * sizeof *r->children); }
    h->nchild = 0;                                   /* references moved into the recording */
    memcpy(data, h->buf, h->len);
    r->data = data;
    atomic_init(&r->dead, 0);
    atomic_init(&r->last_use, cmdrec_now());
    /* the command memory this list occupies (from the start of the current chunk when memory was added while recording) */
    uintptr_t lo = h->rec_lo >= h->cmd_base && h->rec_lo <= h->cmd_base + h->used_est ? h->rec_lo : h->cmd_base;
    r->lo = lo;
    r->hi = h->cmd_base + h->used_est > lo ? h->cmd_base + h->used_est : lo + 1;
    if (!h->cmd_base) {              /* no command memory was handed over (the callback model): the list has no address, so nothing can overwrite or free it but its handle */
        static _Atomic uint64_t synth;
        r->lo = (1ull << 62) + atomic_fetch_add(&synth, 1) * 2;
        r->hi = r->lo + 1;
    }
    cbh_unlock(h);
    r->reg_idx = ~0u;
    r->handle = htab_alloc(r);
    atomic_fetch_add(&g_rec_live, 1);
    atomic_fetch_add(&g_rec_bytes, sizeof *r + r->len);
    pthread_once(&g_packer_once, packer_start);
    reg_publish(r);
    LOGD("EndRecording -> handle %#llx (%u bytes recorded)", (unsigned long long)r->handle, r->len);
    CMDTRACE("end %#llx cb %p [%#lx,%#lx) len %u\n", (unsigned long long)r->handle, (void *)c, (unsigned long)r->lo, (unsigned long)r->hi, r->len);
    return r->handle;
}

/* Called by NVNdevice::FinalizeCommandHandle: the game is done with the handle. */
void gfx_cmdrec_finalize_handle(uint64_t handle) {
    cmdrec *r = cmdrec_acquire(handle);
    if (!r) return;
    gfx_cmdrec_unregister(r);
    cmdrec_release(r);     /* submissions and calling lists in flight hold their own references */
}

static void add_children(NVNcommandBuffer *c, int n, const NVNcommandHandle *hs, int copy) {
    if (n <= 0) return;
    p_call *p = REC_N(c, p_call, OP_CALL, (size_t)n * 8);
    uint64_t *list = (uint64_t *)(p + 1);
    uint32_t cnt = 0;
    for (int i = 0; i < n; i++) {
        cmdrec *r = cmdrec_acquire(hs[i]);
        if (!r) {
            static _Atomic uint64_t n_stale;
            uint64_t k = atomic_fetch_add(&n_stale, 1);
            CMDTRACE("stale %#llx called in cb %p\n", (unsigned long long)hs[i], (void *)c);
            if (k < 20 || (k & (k - 1)) == 0) LOGW("%s: stale or invalid command handle %#llx ignored (%llu so far; dropped: ceiling %llu, address overwritten %llu, handle finalized %llu, pool freed %llu)", copy ? "CopyCommands" : "CallCommands", (unsigned long long)hs[i], (unsigned long long)k + 1,
                                                   (unsigned long long)g_rel_why[0], (unsigned long long)g_rel_why[1], (unsigned long long)g_rel_why[2], (unsigned long long)g_rel_why[3]);
            continue;
        }
        atomic_store_explicit(&r->last_use, cmdrec_now(), memory_order_relaxed);
        cmdbuf_host *h = cbh_lock((const cmd_t *)c);                /* the recording holds the reference; none when nothing is being recorded */
        if (!h || !((const cmd_t *)c)->recording) { if (h) cbh_unlock(h); cmdrec_release(r); continue; }
        if (h->nchild == h->capchild) {
            uint32_t nc = h->capchild ? h->capchild * 2 : 16;
            cmdrec **nch = realloc(h->children, nc * sizeof *nch);
            if (!nch) gfx_die("out of memory recording a command call");
            h->children = nch; h->capchild = nc;
        }
        h->children[h->nchild++] = r;
        cbh_unlock(h);
        list[cnt++] = (uint64_t)(uintptr_t)r;
    }
    p->count = cnt;
}
NVN(nvnCommandBufferCallCommands, void, NVNcommandBuffer *c, int n, const NVNcommandHandle *hs) { add_children(c, n, hs, 0); }
NVN(nvnCommandBufferCopyCommands, void, NVNcommandBuffer *c, int n, const NVNcommandHandle *hs) { add_children(c, n, hs, 1); }

/* ---------------------------------------------------------------------------------------------------------------
 * State binding
 * --------------------------------------------------------------------------------------------------------------- */
int gfx_program_sample_shading(const NVNprogram *p, float *rate); /* api_program.c */
NVN(nvnCommandBufferBindProgram, void, NVNcommandBuffer *c, const NVNprogram *prog, int stages) {
    p_bind_program *p = REC(c, p_bind_program, OP_BIND_PROGRAM);
    const prog_t *pr = (const prog_t *)prog;
    memset(p, 0, sizeof *p);
    if (pr && pr->magic == PROG_MAGIC) memcpy(p->hash, pr->hash, sizeof p->hash);
    p->stage_mask = (uint32_t)stages;
    if (pr && pr->magic == PROG_MAGIC && (stages & NVN_SHADER_STAGE_FRAGMENT_BIT)) {
        float rate = 0;
        p->sample_shading = gfx_program_sample_shading(prog, &rate) && rate > 0 ? 1u : 0u;
    }
}
NVN(nvnCommandBufferBindBlendState, void, NVNcommandBuffer *c, const NVNblendState *s) { memcpy(REC(c, blend_st, OP_BIND_BLEND), s, sizeof(blend_st)); }
NVN(nvnCommandBufferBindChannelMaskState, void, NVNcommandBuffer *c, const NVNchannelMaskState *s) { memcpy(REC(c, chmask_st, OP_BIND_CHMASK), s, sizeof(chmask_st)); }
NVN(nvnCommandBufferBindColorState, void, NVNcommandBuffer *c, const NVNcolorState *s) { memcpy(REC(c, color_st, OP_BIND_COLOR), s, sizeof(color_st)); }
NVN(nvnCommandBufferBindMultisampleState, void, NVNcommandBuffer *c, const NVNmultisampleState *s) { memcpy(REC(c, ms_st, OP_BIND_MS), s, sizeof(ms_st)); }
NVN(nvnCommandBufferBindPolygonState, void, NVNcommandBuffer *c, const NVNpolygonState *s) { memcpy(REC(c, poly_st, OP_BIND_POLY), s, sizeof(poly_st)); }
NVN(nvnCommandBufferBindDepthStencilState, void, NVNcommandBuffer *c, const NVNdepthStencilState *s) { memcpy(REC(c, ds_st, OP_BIND_DS), s, sizeof(ds_st)); }
NVN(nvnCommandBufferBindVertexAttribState, void, NVNcommandBuffer *c, int n, const NVNvertexAttribState *s) {
    if (n < 0) n = 0; if (n > 32) n = 32;
    p_range *p = REC_N(c, p_range, OP_BIND_VATTR, (size_t)n * sizeof(va_st));
    p->first = 0; p->n = (uint32_t)n;
    memcpy(p + 1, s, (size_t)n * sizeof(va_st));
}
NVN(nvnCommandBufferBindVertexStreamState, void, NVNcommandBuffer *c, int n, const NVNvertexStreamState *s) {
    if (n < 0) n = 0; if (n > 32) n = 32;
    p_range *p = REC_N(c, p_range, OP_BIND_VSTREAM, (size_t)n * sizeof(vs_st));
    p->first = 0; p->n = (uint32_t)n;
    memcpy(p + 1, s, (size_t)n * sizeof(vs_st));
}
NVN(nvnCommandBufferBindVertexBuffer, void, NVNcommandBuffer *c, int idx, NVNbufferAddress a, size_t size) {
    p_range *p = REC_N(c, p_range, OP_BIND_VBUF, sizeof(bufrange));
    p->first = (uint32_t)idx; p->n = 1;
    *(bufrange *)(p + 1) = (bufrange){ a, size };
}
NVN(nvnCommandBufferBindVertexBuffers, void, NVNcommandBuffer *c, int first, int n, const NVNbufferRange *r) {
    if (n < 0) n = 0; if (n > 32) n = 32;
    p_range *p = REC_N(c, p_range, OP_BIND_VBUF, (size_t)n * sizeof(bufrange));
    p->first = (uint32_t)first; p->n = (uint32_t)n;
    memcpy(p + 1, r, (size_t)n * sizeof(bufrange));
}
static void bind_ranges(NVNcommandBuffer *c, uint16_t op, NVNshaderStage st, int first, int n, const NVNbufferRange *r) {
    if (n < 0) n = 0; if (n > 32) n = 32;
    p_stage_range *p = REC_N(c, p_stage_range, op, (size_t)n * sizeof(bufrange));
    p->stage = (uint32_t)st; p->first = (uint32_t)first; p->n = (uint32_t)n;
    memcpy(p + 1, r, (size_t)n * sizeof(bufrange));
}
NVN(nvnCommandBufferBindUniformBuffer, void, NVNcommandBuffer *c, NVNshaderStage st, int idx, NVNbufferAddress a, size_t size) {
    NVNbufferRange r = { a, size }; bind_ranges(c, OP_BIND_UBO, st, idx, 1, &r);
}
NVN(nvnCommandBufferBindUniformBuffers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *r) { bind_ranges(c, OP_BIND_UBO, st, first, n, r); }
NVN(nvnCommandBufferBindStorageBuffer, void, NVNcommandBuffer *c, NVNshaderStage st, int idx, NVNbufferAddress a, size_t size) {
    NVNbufferRange r = { a, size }; bind_ranges(c, OP_BIND_SSBO, st, idx, 1, &r);
}
NVN(nvnCommandBufferBindStorageBuffers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *r) { bind_ranges(c, OP_BIND_SSBO, st, first, n, r); }
static void bind_handles(NVNcommandBuffer *c, uint16_t op, NVNshaderStage st, int first, int n, const uint64_t *hs) {
    if (n < 0) n = 0; if (n > 128) n = 128;
    p_stage_range *p = REC_N(c, p_stage_range, op, (size_t)n * 8);
    p->stage = (uint32_t)st; p->first = (uint32_t)first; p->n = (uint32_t)n;
    memcpy(p + 1, hs, (size_t)n * 8);
}
NVN(nvnCommandBufferBindTexture, void, NVNcommandBuffer *c, NVNshaderStage st, int idx, NVNtextureHandle h) { bind_handles(c, OP_BIND_TEX, st, idx, 1, &h); }
NVN(nvnCommandBufferBindTextures, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNtextureHandle *h) { bind_handles(c, OP_BIND_TEX, st, first, n, h); }
NVN(nvnCommandBufferBindImage, void, NVNcommandBuffer *c, NVNshaderStage st, int idx, NVNimageHandle h) { bind_handles(c, OP_BIND_IMG, st, idx, 1, &h); }
NVN(nvnCommandBufferBindImages, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNimageHandle *h) { bind_handles(c, OP_BIND_IMG, st, first, n, h); }
NVN(nvnCommandBufferBindSeparateTexture, void, NVNcommandBuffer *c, NVNshaderStage st, int idx, NVNseparateTextureHandle h) { bind_handles(c, OP_BIND_SEPTEX, st, idx, 1, &h.value); }
NVN(nvnCommandBufferBindSeparateSampler, void, NVNcommandBuffer *c, NVNshaderStage st, int idx, NVNseparateSamplerHandle h) { bind_handles(c, OP_BIND_SEPSAMP, st, idx, 1, &h.value); }
NVN(nvnCommandBufferBindSeparateTextures, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateTextureHandle *h) { bind_handles(c, OP_BIND_SEPTEX, st, first, n, &h->value); }
NVN(nvnCommandBufferBindSeparateSamplers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateSamplerHandle *h) { bind_handles(c, OP_BIND_SEPSAMP, st, first, n, &h->value); }

uint64_t gfx_dpool_handle(const void *guest_pool);
NVN(nvnCommandBufferSetTexturePool, void, NVNcommandBuffer *c, const NVNtexturePool *p) { *REC(c, uint64_t, OP_SET_TEXPOOL) = p ? gfx_dpool_handle(p) : 0; }
NVN(nvnCommandBufferSetSamplerPool, void, NVNcommandBuffer *c, const NVNsamplerPool *p) { *REC(c, uint64_t, OP_SET_SAMPPOOL) = p ? gfx_dpool_handle(p) : 0; }

static void texref_fill(texref *t, const NVNtexture *tex, const NVNtextureView *v) {
    memset(t, 0, sizeof *t);
    if (tex && ((const tex_t *)tex)->magic == TEX_MAGIC) t->s = ((const tex_t *)tex)->s;
    if (v) { t->view = *(const view_t *)v; t->has_view = 1; }
}
NVN(nvnCommandBufferSetRenderTargets, void, NVNcommandBuffer *c, int n, const NVNtexture *const *colors, const NVNtextureView *const *views, const NVNtexture *depth, const NVNtextureView *dview) {
    p_set_rt *p = REC(c, p_set_rt, OP_SET_RT);
    memset(p, 0, sizeof *p);
    if (n < 0) n = 0; if (n > 8) n = 8;
    p->n = (uint32_t)n;
    for (int i = 0; i < n; i++) texref_fill(&p->color[i], colors ? colors[i] : NULL, views ? views[i] : NULL);
    texref_fill(&p->depth, depth, dview);
}

/* ---------------------------------------------------------------------------------------------------------------
 * Dynamic state
 * --------------------------------------------------------------------------------------------------------------- */
NVN(nvnCommandBufferSetViewport, void, NVNcommandBuffer *c, int x, int y, int w, int h) {
    p_range *p = REC_N(c, p_range, OP_VIEWPORTS, 16);
    p->first = 0; p->n = 1;
    float *f = (float *)(p + 1); f[0] = (float)x; f[1] = (float)y; f[2] = (float)w; f[3] = (float)h;
}
NVN(nvnCommandBufferSetViewports, void, NVNcommandBuffer *c, int first, int n, const float *v) {
    if (n < 0) n = 0; if (n > 16) n = 16;
    p_range *p = REC_N(c, p_range, OP_VIEWPORTS, (size_t)n * 16);
    p->first = (uint32_t)first; p->n = (uint32_t)n;
    memcpy(p + 1, v, (size_t)n * 16);
}
NVN(nvnCommandBufferSetScissor, void, NVNcommandBuffer *c, int x, int y, int w, int h) {
    p_range *p = REC_N(c, p_range, OP_SCISSORS, 16);
    p->first = 0; p->n = 1;
    int32_t *f = (int32_t *)(p + 1); f[0] = x; f[1] = y; f[2] = w; f[3] = h;
}
NVN(nvnCommandBufferSetScissors, void, NVNcommandBuffer *c, int first, int n, const int *v) {
    if (n < 0) n = 0; if (n > 16) n = 16;
    p_range *p = REC_N(c, p_range, OP_SCISSORS, (size_t)n * 16);
    p->first = (uint32_t)first; p->n = (uint32_t)n;
    memcpy(p + 1, v, (size_t)n * 16);
}
NVN(nvnCommandBufferSetDepthRange, void, NVNcommandBuffer *c, float n, float f) {
    p_range *p = REC_N(c, p_range, OP_DEPTH_RANGES, 8);
    p->first = 0; p->n = 1;
    float *d = (float *)(p + 1); d[0] = n; d[1] = f;
}
NVN(nvnCommandBufferSetDepthRanges, void, NVNcommandBuffer *c, int first, int n, const float *v) {
    if (n < 0) n = 0; if (n > 16) n = 16;
    p_range *p = REC_N(c, p_range, OP_DEPTH_RANGES, (size_t)n * 8);
    p->first = (uint32_t)first; p->n = (uint32_t)n;
    memcpy(p + 1, v, (size_t)n * 8);
}
NVN(nvnCommandBufferSetBlendColor, void, NVNcommandBuffer *c, const float *v) { memcpy(rec_op(c, OP_BLEND_COLOR, 16), v, 16); }
NVN(nvnCommandBufferSetStencilValueMask, void, NVNcommandBuffer *c, NVNface f, int v) { uint32_t *p = (uint32_t *)rec_op(c, OP_STENCIL, 12); p[0] = 2; p[1] = (uint32_t)f; p[2] = (uint32_t)v; }
NVN(nvnCommandBufferSetStencilMask, void, NVNcommandBuffer *c, NVNface f, int v) { uint32_t *p = (uint32_t *)rec_op(c, OP_STENCIL, 12); p[0] = 1; p[1] = (uint32_t)f; p[2] = (uint32_t)v; }
NVN(nvnCommandBufferSetStencilRef, void, NVNcommandBuffer *c, NVNface f, int v) { uint32_t *p = (uint32_t *)rec_op(c, OP_STENCIL, 12); p[0] = 0; p[1] = (uint32_t)f; p[2] = (uint32_t)v; }
NVN(nvnCommandBufferSetPolygonOffsetClamp, void, NVNcommandBuffer *c, float factor, float units, float clamp) { float *p = (float *)rec_op(c, OP_POLY_OFFSET, 12); p[0] = factor; p[1] = units; p[2] = clamp; }
NVN(nvnCommandBufferSetLineWidth, void, NVNcommandBuffer *c, float w) { *(float *)rec_op(c, OP_LINE_WIDTH, 4) = w; }
NVN(nvnCommandBufferSetPointSize, void, NVNcommandBuffer *c, float s) { *(float *)rec_op(c, OP_POINT_SIZE, 4) = s; }
NVN(nvnCommandBufferSetPrimitiveRestart, void, NVNcommandBuffer *c, uint32_t en, int idx) { uint32_t *p = (uint32_t *)rec_op(c, OP_PRIM_RESTART, 8); p[0] = B(en); p[1] = (uint32_t)idx; }
NVN(nvnCommandBufferSetAlphaRef, void, NVNcommandBuffer *c, float v) { *(float *)rec_op(c, OP_ALPHA_REF, 4) = v; }
NVN(nvnCommandBufferSetSampleMask, void, NVNcommandBuffer *c, int m) { *(uint32_t *)rec_op(c, OP_SAMPLE_MASK, 4) = (uint32_t)m; }
NVN(nvnCommandBufferSetDepthClamp, void, NVNcommandBuffer *c, uint32_t e) { *(uint32_t *)rec_op(c, OP_DEPTH_CLAMP, 4) = B(e); }
NVN(nvnCommandBufferSetRasterizerDiscard, void, NVNcommandBuffer *c, uint32_t e) { *(uint32_t *)rec_op(c, OP_RASTER_DISCARD, 4) = B(e); }
NVN(nvnCommandBufferSetDepthBounds, void, NVNcommandBuffer *c, uint32_t e, float mn, float mx) { float *p = (float *)rec_op(c, OP_DEPTH_BOUNDS, 12); ((uint32_t *)p)[0] = B(e); p[1] = mn; p[2] = mx; }
NVN(nvnCommandBufferSetRenderEnable, void, NVNcommandBuffer *c, uint32_t e) { uint32_t *p = (uint32_t *)rec_op(c, OP_RENDER_ENABLE, 16); p[0] = B(e); p[1] = 0; *(uint64_t *)(p + 2) = 0; }
NVN(nvnCommandBufferSetRenderEnableConditional, void, NVNcommandBuffer *c, NVNconditionalRenderMode m, NVNbufferAddress a) {
    uint32_t *p = (uint32_t *)rec_op(c, OP_RENDER_ENABLE, 16); p[0] = 1; p[1] = (uint32_t)m + 1; *(uint64_t *)(p + 2) = a;
}
/* Accepted and ignored: no Metal equivalent or no visible effect. */
NVN(nvnCommandBufferSetTiledCacheAction, void, NVNcommandBuffer *c, NVNtiledCacheAction a) { (void)c; (void)a; }
NVN(nvnCommandBufferSetTiledCacheTileSize, void, NVNcommandBuffer *c, int w, int h) { (void)c; (void)w; (void)h; }
NVN(nvnCommandBufferSetConservativeRasterEnable, void, NVNcommandBuffer *c, uint32_t e) { (void)c; (void)e; }
NVN(nvnCommandBufferSetConservativeRasterDilate, void, NVNcommandBuffer *c, float d) { (void)c; (void)d; }
NVN(nvnCommandBufferSetSubpixelPrecisionBias, void, NVNcommandBuffer *c, int x, int y) { (void)c; (void)x; (void)y; }
NVN(nvnCommandBufferSetViewportSwizzles, void, NVNcommandBuffer *c, int f, int n, const NVNviewportSwizzle *s) { (void)c; (void)f; (void)n; (void)s; }
NVN(nvnCommandBufferSetShaderScratchMemory, void, NVNcommandBuffer *c, const NVNmemoryPool *p, ptrdiff_t o, size_t s) { (void)c; (void)p; (void)o; (void)s; }
NVN(nvnCommandBufferSaveZCullData, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t s) { (void)c; (void)a; (void)s; }
NVN(nvnCommandBufferRestoreZCullData, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t s) { (void)c; (void)a; (void)s; }
/* Tessellation state: patch size (control points per input patch) and the default levels used when no tess control program writes them. */
NVN(nvnCommandBufferSetPatchSize, void, NVNcommandBuffer *c, int s) { *(uint32_t *)rec_op(c, OP_PATCH_SIZE, 4) = (uint32_t)(s < 1 ? 1 : s > 32 ? 32 : s); }
NVN(nvnCommandBufferSetInnerTessellationLevels, void, NVNcommandBuffer *c, const float *v) {
    p_tess_levels *p = REC(c, p_tess_levels, OP_TESS_LEVELS); p->outer = 0; p->pad = 0; p->v[0] = v[0]; p->v[1] = v[1]; p->v[2] = p->v[3] = 0;
}
NVN(nvnCommandBufferSetOuterTessellationLevels, void, NVNcommandBuffer *c, const float *v) {
    p_tess_levels *p = REC(c, p_tess_levels, OP_TESS_LEVELS); p->outer = 1; p->pad = 0; memcpy(p->v, v, 16);
}
/* Transform feedback: buffer bindings, begin / end / pause / resume with the game's control memory, and the draw that takes its vertex count from it. */
NVN(nvnCommandBufferBindTransformFeedbackBuffer, void, NVNcommandBuffer *c, int idx, NVNbufferAddress a, size_t size) {
    if (idx < 0 || idx > 3) return;
    p_range *p = REC_N(c, p_range, OP_BIND_XFB, sizeof(bufrange));
    p->first = (uint32_t)idx; p->n = 1;
    *(bufrange *)(p + 1) = (bufrange){ a, size };
}
NVN(nvnCommandBufferBindTransformFeedbackBuffers, void, NVNcommandBuffer *c, int first, int n, const NVNbufferRange *r) {
    if (first < 0 || first > 3) return;
    if (n < 0) n = 0; if (first + n > 4) n = 4 - first;
    p_range *p = REC_N(c, p_range, OP_BIND_XFB, (size_t)n * sizeof(bufrange));
    p->first = (uint32_t)first; p->n = (uint32_t)n;
    memcpy(p + 1, r, (size_t)n * sizeof(bufrange));
}
static void xfb_op(NVNcommandBuffer *c, uint32_t op, NVNbufferAddress ctl) { *REC(c, p_xfb, OP_XFB) = (p_xfb){ op, 0, ctl }; }
NVN(nvnCommandBufferBeginTransformFeedback, void, NVNcommandBuffer *c, NVNbufferAddress a) { xfb_op(c, XFB_BEGIN, a); }
NVN(nvnCommandBufferEndTransformFeedback, void, NVNcommandBuffer *c, NVNbufferAddress a) { xfb_op(c, XFB_END, a); }
NVN(nvnCommandBufferPauseTransformFeedback, void, NVNcommandBuffer *c, NVNbufferAddress a) { xfb_op(c, XFB_PAUSE, a); }
NVN(nvnCommandBufferResumeTransformFeedback, void, NVNcommandBuffer *c, NVNbufferAddress a) { xfb_op(c, XFB_RESUME, a); }
NVN(nvnCommandBufferDrawTransformFeedback, void, NVNcommandBuffer *c, NVNdrawPrimitive m, NVNbufferAddress a) { *REC(c, p_draw_xfb, OP_DRAW_XFB) = (p_draw_xfb){ (uint32_t)m, 0, a }; }
NVN(nvnCommandBufferBindCoverageModulationTable, void, NVNcommandBuffer *c, const float *v) { (void)c; (void)v; }
NVN(nvnCommandBufferSetColorReductionEnable, void, NVNcommandBuffer *c, uint32_t e) { (void)c; (void)e; }
NVN(nvnCommandBufferSetColorReductionThresholds, void, NVNcommandBuffer *c, int a, int b) { (void)c; (void)a; (void)b; }
NVN(nvnCommandBufferSetStencilCullCriteria, void, NVNcommandBuffer *c, int a, int b, int d) { (void)c; (void)a; (void)b; (void)d; }
NVN(nvnCommandBufferSetProgramSubroutines, void, NVNcommandBuffer *c, NVNprogram *p, NVNshaderStage s, const int a, const int b, const int *v) { (void)c; (void)p; (void)s; (void)a; (void)b; (void)v; }

/* ---------------------------------------------------------------------------------------------------------------
 * Clears, draws, dispatch
 * --------------------------------------------------------------------------------------------------------------- */
static void clear_color(NVNcommandBuffer *c, int idx, const void *v, int mask, uint32_t kind) {
    p_clear_color *p = REC(c, p_clear_color, OP_CLEAR_COLOR);
    p->index = (uint32_t)idx; p->kind = kind; p->mask = (uint32_t)mask & 0xf; p->pad = 0;
    memcpy(p->v, v, 16);
}
NVN(nvnCommandBufferClearColor, void, NVNcommandBuffer *c, int i, const float *v, int m) { clear_color(c, i, v, m, 0); }
NVN(nvnCommandBufferClearColori, void, NVNcommandBuffer *c, int i, const int *v, int m) { clear_color(c, i, v, m, 1); }
NVN(nvnCommandBufferClearColorui, void, NVNcommandBuffer *c, int i, const uint32_t *v, int m) { clear_color(c, i, v, m, 2); }
NVN(nvnCommandBufferClearDepthStencil, void, NVNcommandBuffer *c, float d, uint32_t dm, int s, int sm) {
    p_clear_ds *p = REC(c, p_clear_ds, OP_CLEAR_DS);
    p->depth = d; p->depth_mask = B(dm); p->stencil = (uint32_t)s; p->stencil_mask = (uint32_t)sm;
}
NVN(nvnCommandBufferDiscardColor, void, NVNcommandBuffer *c, int i) { *(uint32_t *)rec_op(c, OP_DISCARD_COLOR, 4) = (uint32_t)i; }
NVN(nvnCommandBufferDiscardDepthStencil, void, NVNcommandBuffer *c) { rec_op(c, OP_DISCARD_DS, 0); }

NVN(nvnCommandBufferDrawArrays, void, NVNcommandBuffer *c, NVNdrawPrimitive m, int first, int count) {
    p_draw_arrays *p = REC(c, p_draw_arrays, OP_DRAW_ARRAYS);
    *p = (p_draw_arrays){ (uint32_t)m, (uint32_t)first, (uint32_t)count, 0, 1, 0 };
}
NVN(nvnCommandBufferDrawArraysInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive m, int first, int count, int base_inst, int n_inst) {
    p_draw_arrays *p = REC(c, p_draw_arrays, OP_DRAW_ARRAYS);
    *p = (p_draw_arrays){ (uint32_t)m, (uint32_t)first, (uint32_t)count, (uint32_t)base_inst, (uint32_t)n_inst, 0 };
}
static void draw_elems(NVNcommandBuffer *c, NVNdrawPrimitive m, NVNindexType t, int count, NVNbufferAddress a, int base_vertex, int base_inst, int n_inst) {
    p_draw_elems *p = REC(c, p_draw_elems, OP_DRAW_ELEMENTS);
    *p = (p_draw_elems){ (uint32_t)m, (uint32_t)t, (uint32_t)count, (uint32_t)base_vertex, (uint32_t)base_inst, (uint32_t)n_inst, a };
}
NVN(nvnCommandBufferDrawElements, void, NVNcommandBuffer *c, NVNdrawPrimitive m, NVNindexType t, int count, NVNbufferAddress a) { draw_elems(c, m, t, count, a, 0, 0, 1); }
NVN(nvnCommandBufferDrawElementsBaseVertex, void, NVNcommandBuffer *c, NVNdrawPrimitive m, NVNindexType t, int count, NVNbufferAddress a, int bv) { draw_elems(c, m, t, count, a, bv, 0, 1); }
NVN(nvnCommandBufferDrawElementsInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive m, NVNindexType t, int count, NVNbufferAddress a, int bv, int bi, int ni) { draw_elems(c, m, t, count, a, bv, bi, ni); }
NVN(nvnCommandBufferDrawArraysIndirect, void, NVNcommandBuffer *c, NVNdrawPrimitive m, NVNbufferAddress ind) {
    p_draw_ind *p = REC(c, p_draw_ind, OP_DRAW_ARRAYS_IND);
    *p = (p_draw_ind){ (uint32_t)m, 0, ind, 0 };
}
NVN(nvnCommandBufferDrawElementsIndirect, void, NVNcommandBuffer *c, NVNdrawPrimitive m, NVNindexType t, NVNbufferAddress idx, NVNbufferAddress ind) {
    p_draw_ind *p = REC(c, p_draw_ind, OP_DRAW_ELEMENTS_IND);
    *p = (p_draw_ind){ (uint32_t)m, (uint32_t)t, ind, idx };
}
/* draw i < min(*count, max) reads its arguments at ind + i * stride (the 32-bit count is read by the GPU when the draws execute) */
NVN(nvnCommandBufferMultiDrawArraysIndirectCount, void, NVNcommandBuffer *c, NVNdrawPrimitive m, NVNbufferAddress ind, NVNbufferAddress count, int max, ptrdiff_t stride) {
    if (max <= 0) return;
    *REC(c, p_draw_ind_count, OP_DRAW_IND_COUNT) = (p_draw_ind_count){ (uint32_t)m, 0, 0, (uint32_t)max, ind, 0, count, (uint64_t)(stride ? stride : 16) };
}
NVN(nvnCommandBufferMultiDrawElementsIndirectCount, void, NVNcommandBuffer *c, NVNdrawPrimitive m, NVNindexType t, NVNbufferAddress idx, NVNbufferAddress ind, NVNbufferAddress count, int max, ptrdiff_t stride) {
    if (max <= 0) return;
    *REC(c, p_draw_ind_count, OP_DRAW_IND_COUNT) = (p_draw_ind_count){ (uint32_t)m, (uint32_t)t, 1, (uint32_t)max, ind, idx, count, (uint64_t)(stride ? stride : 20) };
}
NVN(nvnCommandBufferDispatchCompute, void, NVNcommandBuffer *c, int x, int y, int z) { *REC(c, p_dispatch, OP_DISPATCH) = (p_dispatch){ (uint32_t)x, (uint32_t)y, (uint32_t)z, 0 }; }
NVN(nvnCommandBufferDispatchComputeIndirect, void, NVNcommandBuffer *c, NVNbufferAddress a) { *(uint64_t *)rec_op(c, OP_DISPATCH_IND, 8) = a; }

/* ---------------------------------------------------------------------------------------------------------------
 * Copies and clears of resources
 * --------------------------------------------------------------------------------------------------------------- */
NVN(nvnCommandBufferSetCopyRowStride, void, NVNcommandBuffer *c, ptrdiff_t s) {
    CBH(c)->row_stride = s;
    int64_t *p = (int64_t *)rec_op(c, OP_COPY_STRIDES, 16); p[0] = CBH(c)->row_stride; p[1] = CBH(c)->img_stride;
}
NVN(nvnCommandBufferSetCopyImageStride, void, NVNcommandBuffer *c, ptrdiff_t s) {
    CBH(c)->img_stride = s;
    int64_t *p = (int64_t *)rec_op(c, OP_COPY_STRIDES, 16); p[0] = CBH(c)->row_stride; p[1] = CBH(c)->img_stride;
}
NVN(nvnCommandBufferGetCopyRowStride, ptrdiff_t, const NVNcommandBuffer *c) { return CBH(c)->row_stride; }
NVN(nvnCommandBufferGetCopyImageStride, ptrdiff_t, const NVNcommandBuffer *c) { return CBH(c)->img_stride; }

static void region_copy(int32_t dst[6], const NVNcopyRegion *r) { if (r) memcpy(dst, r, 24); else { memset(dst, 0, 24); } }
NVN(nvnCommandBufferCopyBufferToBuffer, void, NVNcommandBuffer *c, NVNbufferAddress s, NVNbufferAddress d, size_t n, int flags) { (void)flags; *REC(c, p_copy_b2b, OP_COPY_B2B) = (p_copy_b2b){ s, d, n }; }
NVN(nvnCommandBufferCopyBufferToTexture, void, NVNcommandBuffer *c, NVNbufferAddress src, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, int flags) {
    p_copy_xfer *p = REC(c, p_copy_xfer, OP_COPY_B2T);
    memset(p, 0, sizeof *p);
    p->flags = (uint32_t)flags; p->buf = src; region_copy(p->region, r); texref_fill(&p->tex, t, v);
    p->row_stride = (uint64_t)CBH(c)->row_stride; p->img_stride = (uint64_t)CBH(c)->img_stride;
}
NVN(nvnCommandBufferCopyTextureToBuffer, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, NVNbufferAddress dst, int flags) {
    p_copy_xfer *p = REC(c, p_copy_xfer, OP_COPY_T2B);
    memset(p, 0, sizeof *p);
    p->flags = (uint32_t)flags; p->buf = dst; region_copy(p->region, r); texref_fill(&p->tex, t, v);
    p->row_stride = (uint64_t)CBH(c)->row_stride; p->img_stride = (uint64_t)CBH(c)->img_stride;
}
NVN(nvnCommandBufferCopyTextureToTexture, void, NVNcommandBuffer *c, const NVNtexture *st, const NVNtextureView *sv, const NVNcopyRegion *sr, const NVNtexture *dt, const NVNtextureView *dv, const NVNcopyRegion *dr, int flags) {
    p_copy_t2t *p = REC(c, p_copy_t2t, OP_COPY_T2T);
    memset(p, 0, sizeof *p);
    p->flags = (uint32_t)flags; texref_fill(&p->src, st, sv); texref_fill(&p->dst, dt, dv); region_copy(p->sregion, sr); region_copy(p->dregion, dr);
}
NVN(nvnCommandBufferClearBuffer, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n, uint32_t v) {
    uint64_t *p = (uint64_t *)rec_op(c, OP_CLEAR_BUF, 24); p[0] = a; p[1] = n; p[2] = v;
}
static void clear_tex(NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const void *col, int mask, uint32_t kind) {
    p_copy_xfer *p = REC(c, p_copy_xfer, OP_CLEAR_TEX);
    memset(p, 0, sizeof *p);
    p->flags = kind | ((uint32_t)mask << 8); region_copy(p->region, r); texref_fill(&p->tex, t, v);
    memcpy(&p->buf, col, 8); memcpy(&p->row_stride, (const uint8_t *)col + 8, 8);      /* 16 bytes of clear value */
}
NVN(nvnCommandBufferClearTexture, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const float *col, int m) { clear_tex(c, t, v, r, col, m, 0); }
NVN(nvnCommandBufferClearTexturei, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const int *col, int m) { clear_tex(c, t, v, r, col, m, 1); }
NVN(nvnCommandBufferClearTextureui, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const uint32_t *col, int m) { clear_tex(c, t, v, r, col, m, 2); }
NVN(nvnCommandBufferUpdateUniformBuffer, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t bufsize, ptrdiff_t off, size_t size, const void *data) {
    (void)bufsize;
    if (size > (1u << 20)) { LOG_ONCE(NAPPLE_LOG_WARN, "UpdateUniformBuffer of %zu bytes clamped", size); size = 1u << 20; }
    p_update_ubo *p = REC_N(c, p_update_ubo, OP_UPDATE_UBO, size);
    p->addr = a; p->offset = (uint32_t)off; p->size = (uint32_t)size;
    memcpy(p + 1, data, size);
}
static void downsample(NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNtexture *d, const NVNtextureView *dv) {
    p_copy_t2t *p = REC(c, p_copy_t2t, OP_DOWNSAMPLE);
    memset(p, 0, sizeof *p);
    texref_fill(&p->src, s, sv); texref_fill(&p->dst, d, dv);
}
NVN(nvnCommandBufferDownsample, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtexture *d) { downsample(c, s, NULL, d, NULL); }
NVN(nvnCommandBufferTiledDownsample, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtexture *d) { downsample(c, s, NULL, d, NULL); }
NVN(nvnCommandBufferDownsampleTextureView, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNtexture *d, const NVNtextureView *dv) { downsample(c, s, sv, d, dv); }
NVN(nvnCommandBufferTiledDownsampleTextureView, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNtexture *d, const NVNtextureView *dv) { downsample(c, s, sv, d, dv); }
/* Decompresses the bound depth target on the console (needed before sampling it after programmable sample locations). Metal depth targets are never compressed,
 * but the depth of the open render pass only reaches the texture when the pass ends: the replay closes the pass (and flushes pending clears) at this point, so a
 * following sample or copy of the depth buffer sees it. Recorded as a barrier with a private bit. */
NVN(nvnCommandBufferResolveDepthBuffer, void, NVNcommandBuffer *c) { *(uint32_t *)rec_op(c, OP_BARRIER, 4) = BARRIER_RESOLVE_DEPTH; }
NVN(nvnCommandBufferDrawTexture, void, NVNcommandBuffer *c, NVNtextureHandle h, const NVNdrawTextureRegion *dst, const NVNdrawTextureRegion *src) {
    if (!dst) return;
    p_draw_tex *p = REC(c, p_draw_tex, OP_DRAW_TEXTURE);
    p->handle = h;
    memcpy(p->dst, dst, sizeof p->dst);
    if (src) memcpy(p->src, src, sizeof p->src); else { p->src[0] = p->src[1] = 0; p->src[2] = p->src[3] = 1; }
}

/* ---------------------------------------------------------------------------------------------------------------
 * Synchronization, counters, debug
 * --------------------------------------------------------------------------------------------------------------- */
NVN(nvnCommandBufferBarrier, void, NVNcommandBuffer *c, int bits) { *(uint32_t *)rec_op(c, OP_BARRIER, 4) = (uint32_t)bits; }
NVN(nvnCommandBufferWaitSync, void, NVNcommandBuffer *c, const NVNsync *s) { p_sync *p = REC(c, p_sync, OP_WAIT_SYNC); *p = (p_sync){ (uint64_t)(uintptr_t)s, 0, 0 }; }
NVN(nvnCommandBufferFenceSync, void, NVNcommandBuffer *c, NVNsync *s, NVNsyncCondition cond, int flags) {
    sync_t *so = (sync_t *)s;                 /* not signaled until the replay reaches the fence: waiters must see it pending right away */
    so->kind = 0; so->cell = NULL; so->seq = UINT64_MAX;
    p_sync *p = REC(c, p_sync, OP_FENCE_SYNC); *p = (p_sync){ (uint64_t)(uintptr_t)s, (uint32_t)cond, (uint32_t)flags }; }
NVN(nvnCommandBufferSignalEvent, void, NVNcommandBuffer *c, const NVNevent *e, NVNeventSignalMode m, NVNeventSignalLocation loc, int flags, uint32_t v) {
    (void)loc; (void)flags;
    p_event *p = REC(c, p_event, OP_SIGNAL_EVENT);
    *p = (p_event){ (uint64_t)(uintptr_t)((const ev_t *)e)->val, (uint32_t)m, v, 0 };
}
NVN(nvnCommandBufferWaitEvent, void, NVNcommandBuffer *c, const NVNevent *e, NVNeventWaitMode m, uint32_t v) {
    p_event *p = REC(c, p_event, OP_WAIT_EVENT);
    *p = (p_event){ (uint64_t)(uintptr_t)((const ev_t *)e)->val, (uint32_t)m, v, 0 };
}
NVN(nvnCommandBufferReportCounter, void, NVNcommandBuffer *c, NVNcounterType t, NVNbufferAddress a) { *REC(c, p_counter, OP_REPORT_COUNTER) = (p_counter){ (uint32_t)t, 0, a }; }
NVN(nvnCommandBufferResetCounter, void, NVNcommandBuffer *c, NVNcounterType t) { *(uint32_t *)rec_op(c, OP_RESET_COUNTER, 4) = (uint32_t)t; }
NVN(nvnCommandBufferReportValue, void, NVNcommandBuffer *c, uint32_t v, NVNbufferAddress a) { *REC(c, p_counter, OP_REPORT_COUNTER) = (p_counter){ 0x100, v, a }   /* type 0x100 = ReportValue, pad = the value */; }
NVN(nvnCommandBufferPushDebugGroupStatic, void, NVNcommandBuffer *c, uint32_t id, const char *s) { (void)c; (void)id; (void)s; }
NVN(nvnCommandBufferPushDebugGroupDynamic, void, NVNcommandBuffer *c, uint32_t id, const char *s) { (void)c; (void)id; (void)s; }
NVN(nvnCommandBufferPushDebugGroup, void, NVNcommandBuffer *c, uint32_t id, const char *s) { (void)c; (void)id; (void)s; }
NVN(nvnCommandBufferPopDebugGroup, void, NVNcommandBuffer *c) { (void)c; }
NVN(nvnCommandBufferPopDebugGroupId, void, NVNcommandBuffer *c, uint32_t id) { (void)c; (void)id; }
NVN(nvnCommandBufferInsertDebugMarkerStatic, void, NVNcommandBuffer *c, uint32_t id, const char *s) { (void)c; (void)id; (void)s; }
NVN(nvnCommandBufferInsertDebugMarkerDynamic, void, NVNcommandBuffer *c, uint32_t id, const char *s) { (void)c; (void)id; (void)s; }
NVN(nvnCommandBufferInsertDebugMarker, void, NVNcommandBuffer *c, const char *s) { (void)c; (void)s; }
NVN(nvnCommandBufferInsertCheckpointStatic, void, NVNcommandBuffer *c, uint32_t id, const char *s) { (void)c; (void)id; (void)s; }
NVN(nvnCommandBufferInsertCheckpointDynamic, void, NVNcommandBuffer *c, uint32_t id, const char *s) { (void)c; (void)id; (void)s; }

