#include <stdio.h>
/* api_memory.c - NVN memory pools and buffers. Pool memory is guest memory: the GPU address of a byte is its host address. */
#include <stdlib.h>
#include <unistd.h>
#include <mach/mach.h>
#include <TargetConditionals.h>
#if TARGET_OS_IOS
/* iOS has no <mach/mach_vm.h>: vm_remap has the same signature with 64-bit address types on arm64 */
#include <mach/vm_map.h>
#define mach_vm_address_t vm_address_t /* uint64_t in the iOS headers, but vm_remap takes a vm_address_t pointer */
#define mach_vm_remap vm_remap
#else
#include <mach/mach_vm.h>
#endif
#include <sys/mman.h>
#include "gfx_mtl.h"
#include "timeline.h"

void gfx_api_link_memory(void) {}

#define POOL_MAGIC MAGIC('P', 'O', 'O', 'L')
#define BUF_MAGIC MAGIC('B', 'U', 'F', '0')
#define PB(b) ((pool_builder_t *)(b))
#define CPB(b) ((const pool_builder_t *)(b))

NVN(nvnMemoryPoolBuilderSetDevice, void, NVNmemoryPoolBuilder *b, NVNdevice *d) { PB(b)->dev = d; }
NVN(nvnMemoryPoolBuilderSetDefaults, void, NVNmemoryPoolBuilder *b) {
    NVNdevice *d = PB(b)->dev;
    memset(b, 0, sizeof(pool_builder_t));
    PB(b)->dev = d;
    PB(b)->flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED;   /* documented NVN default */
}
NVN(nvnMemoryPoolBuilderSetStorage, void, NVNmemoryPoolBuilder *b, void *m, size_t n) { PB(b)->mem = m; PB(b)->size = n; }
NVN(nvnMemoryPoolBuilderSetFlags, void, NVNmemoryPoolBuilder *b, int f) { PB(b)->flags = (uint32_t)f; }
NVN(nvnMemoryPoolBuilderGetDevice, const NVNdevice *, const NVNmemoryPoolBuilder *b) { return CPB(b)->dev; }
NVN(nvnMemoryPoolBuilderGetMemory, void *, const NVNmemoryPoolBuilder *b) { return CPB(b)->mem; }
NVN(nvnMemoryPoolBuilderGetSize, size_t, const NVNmemoryPoolBuilder *b) { return (size_t)CPB(b)->size; }
NVN(nvnMemoryPoolBuilderGetFlags, int, const NVNmemoryPoolBuilder *b) { return (int)CPB(b)->flags; }

static pool_t *g_allp[8192]; static uint32_t g_nallp;
/* Shader code pools, newest last: ProgramSetShaders may name code by a bare pool offset (see gfx_find_code_pool). */
#include <pthread.h>
static pthread_mutex_t g_cp_mu = PTHREAD_MUTEX_INITIALIZER;
static pool_t *g_cp[256]; static uint64_t g_cps[256]; static uint32_t g_ncp; static uint64_t g_cp_seq;
uint64_t gfx_code_pool_seq(void) { return g_cp_seq; }
static void cp_add(pool_t *o) {
    pthread_mutex_lock(&g_cp_mu);
    for (uint32_t i = 0; i < g_ncp; i++) if (g_cp[i] == o) { memmove(g_cp + i, g_cp + i + 1, (g_ncp - i - 1) * sizeof *g_cp); memmove(g_cps + i, g_cps + i + 1, (g_ncp - i - 1) * sizeof *g_cps); g_ncp--; break; }
    if (g_ncp == 256) { memmove(g_cp, g_cp + 1, 255 * sizeof *g_cp); memmove(g_cps, g_cps + 1, 255 * sizeof *g_cps); g_ncp--; }
    g_cps[g_ncp] = ++g_cp_seq; g_cp[g_ncp++] = o;
    pthread_mutex_unlock(&g_cp_mu);
}
static void cp_remove(pool_t *o) {
    pthread_mutex_lock(&g_cp_mu);
    for (uint32_t i = 0; i < g_ncp; i++) if (g_cp[i] == o) { memmove(g_cp + i, g_cp + i + 1, (g_ncp - i - 1) * sizeof *g_cp); memmove(g_cps + i, g_cps + i + 1, (g_ncp - i - 1) * sizeof *g_cps); g_ncp--; break; }
    pthread_mutex_unlock(&g_cp_mu);
}
/* The oldest shader-code pool that has an NVN shader header (magic 0x12345678) at every one of the `n` offsets of a program: a program's stages come from one
 * shader archive, so one pool holds all of them (a single offset matches several pools by chance). Returns the pool's memory, NULL when none matches (yet). */
const uint8_t *gfx_find_code_pool_multi(const uint64_t *offs, const uint64_t *need, const int *stage, int n) {
    const uint8_t *r = NULL;
    pthread_mutex_lock(&g_cp_mu);
    for (uint32_t i = 0; i < g_ncp && !r; i++) {
        const pool_t *o = g_cp[i];
        int ok = o->mem && !(o->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS);
        for (int k = 0; ok && k < n; k++) {
            ok = offs[k] + need[k] <= o->size && *(const uint32_t *)(o->mem + offs[k]) == 0x12345678u;
            /* the program header (SPH, at +0x30) names the stage in its low 5 bits: 1 vertex, 2 fragment (seen on Animal Crossing's shaders); a header of another stage is another program's code */
            {
                uint32_t sw = ok ? *(const uint32_t *)(o->mem + offs[k] + 0x30) : 0;
                if (sw && (stage[k] == 0 || stage[k] == 1) && (sw & 0x1f) != (uint32_t)(stage[k] + 1)) ok = 0;
            }
        }
        if (ok) r = o->mem;
    }
    pthread_mutex_unlock(&g_cp_mu);
    return r;
}
/* A shader's code starts with the NVN shader header magic. The pool for a bare offset: the first shader-code pool created after sequence number `after` that has a
 * header at `off` (the usual case: the game creates the pool right after the program) (an older pool is never guessed: the code is not in it). */
const uint8_t *gfx_find_code_pool(uint64_t off, uint64_t need, uint64_t after) {
    const uint8_t *r = NULL;
    pthread_mutex_lock(&g_cp_mu);
    for (int pass = 0; pass < 1 && !r; pass++)
        for (int k = 0; k < (int)g_ncp && !r; k++) {
            int i = pass == 0 ? k : (int)g_ncp - 1 - k;
            pool_t *o = g_cp[i];
            if ((pass == 0) != (g_cps[i] > after) || !o->mem || (o->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) || off + need > o->size) continue;
            if (*(const uint32_t *)(o->mem + off) == 0x12345678u) { if (getenv("NAPPLE_POOLLOG")) fprintf(stderr, "[poollog] code pool candidate for off %#llx (after seq %llu): seq %llu pool %p mem %p size %#llx flags %#x pass %d\n", (unsigned long long)off, (unsigned long long)after, (unsigned long long)g_cps[i], (void *)o, (void *)o->mem, (unsigned long long)o->size, o->flags, pass); r = o->mem + off; }
        }
    if (!r && getenv("NAPPLE_POOLLOG") && off == 0xe00) {
        static int k; if (k++ < 3) for (uint32_t i = 0; i < g_nallp; i++) { const pool_t *o = g_allp[i]; if (o->mem && !(o->flags & 1) && off + 4 <= o->size && *(const uint32_t *)(o->mem + off) == 0x12345678u) fprintf(stderr, "[poollog] ANY-pool header at %#llx: pool %p mem %p size %#llx flags %#x idx %u\n", (unsigned long long)off, (const void *)o, (void *)o->mem, (unsigned long long)o->size, o->flags, i); }
    }
    if (!r && getenv("NAPPLE_POOLLOG"))
        for (uint32_t i = 0; i < g_ncp; i++) if (g_cps[i] > after && g_cp[i]->mem && off + 16 <= g_cp[i]->size) { const uint32_t *w = (const uint32_t *)(g_cp[i]->mem + off); fprintf(stderr, "[poollog] no header at off %#llx in pool seq %llu mem %p size %#llx flags %#x: %08x %08x %08x %08x\n", (unsigned long long)off, (unsigned long long)g_cps[i], (void *)g_cp[i]->mem, (unsigned long long)g_cp[i]->size, g_cp[i]->flags, w[0], w[1], w[2], w[3]); }
    pthread_mutex_unlock(&g_cp_mu);
    return r;
}
/* Every live pool, for diagnostics: which pool (if any) holds an address the replay could not resolve. */
static void ap_add(pool_t *o) { pthread_mutex_lock(&g_cp_mu); if (g_nallp < 8192) g_allp[g_nallp++] = o; pthread_mutex_unlock(&g_cp_mu); }
static struct { uint64_t lo, hi; uint32_t flags; } g_dead[64]; static uint32_t g_ndead;
static void ap_remove2(pool_t *o);
static void ap_remove(pool_t *o) {
    pthread_mutex_lock(&g_cp_mu); g_dead[g_ndead++ & 63] = (typeof(g_dead[0])){ (uint64_t)(uintptr_t)o->mem, (uint64_t)(uintptr_t)o->mem + o->size, o->flags }; pthread_mutex_unlock(&g_cp_mu);
    ap_remove2(o);
}
static void ap_remove2(pool_t *o) { pthread_mutex_lock(&g_cp_mu); for (uint32_t i = 0; i < g_nallp; i++) if (g_allp[i] == o) { g_allp[i] = g_allp[--g_nallp]; break; } pthread_mutex_unlock(&g_cp_mu); }
void gfx_pool_describe(uint64_t addr, char *out, size_t n) {
    out[0] = 0;
    pthread_mutex_lock(&g_cp_mu);
    for (uint32_t i = 0; i < g_nallp; i++) {
        const pool_t *o = g_allp[i];
        if (o->magic == POOL_MAGIC && o->mem && addr >= (uint64_t)(uintptr_t)o->mem && addr < (uint64_t)(uintptr_t)o->mem + o->size) {
            snprintf(out, n, "inside pool %p (%p+%#llx flags %#x%s)", (const void *)o, (void *)o->mem, (unsigned long long)o->size, o->flags, o->mtl ? "" : ", NOT registered with Metal");
            break;
        }
    }
    if (!out[0]) for (uint32_t i = 0; i < 64 && i < g_ndead; i++) if (addr >= g_dead[i].lo && addr < g_dead[i].hi) { snprintf(out, n, "inside a FINALIZED pool (%#llx+%#llx flags %#x)", (unsigned long long)g_dead[i].lo, (unsigned long long)(g_dead[i].hi - g_dead[i].lo), g_dead[i].flags); break; }
    pthread_mutex_unlock(&g_cp_mu);
}
static uint64_t g_last_phys_mis;      /* start address of the most recent physical pool (its misalignment is copied by virtual pools) */
NVN(nvnMemoryPoolInitialize, NVNboolean, NVNmemoryPool *p, const NVNmemoryPoolBuilder *b) {
    const pool_builder_t *s = CPB(b);
    pool_t *o = (pool_t *)p;
    memset(o, 0, sizeof *o);
    if (s->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) {
        /* A virtual pool is a GPU address range without storage. The GPU address of a byte is its host address here, so reserve a host range (zero-filled,
         * readable, writable: unmapped pages read as zero) and let MapVirtual alias physical pool pages into it. */
        if (!s->size) { LOGE("virtual MemoryPool without a size"); return 0; }
        /* Physical pool storage is only 4 KiB aligned in guest memory while the host maps 16 KiB pages: a range can be aliased by page remapping only when its
         * address and its source agree modulo the host page. Virtual pools are normally paired with the physical pool created just before, so the reservation
         * starts at the same misalignment (the slack pages around the range alias unrelated guest memory that the GPU never addresses). */
        const uint64_t pg = (uint64_t)getpagesize(), slack = 2 * pg;
        uint64_t raw_size = ((s->size + pg - 1) & ~(pg - 1)) + 2 * slack;
        void *m = mmap(NULL, (size_t)raw_size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
        if (m == MAP_FAILED) { LOGE("virtual MemoryPool: cannot reserve %#llx bytes", (unsigned long long)raw_size); return 0; }
        o->magic = POOL_MAGIC; o->flags = s->flags; o->vraw = m; o->vraw_size = raw_size;
        o->mem = (uint8_t *)m + slack + (g_last_phys_mis & (pg - 1)); o->size = s->size;
        ap_add(o);
        LOGD("virtual MemoryPool %p range=%p size=%#llx flags=%#x", (void *)p, m, (unsigned long long)o->size, o->flags);
        return 1;
    }
    if (s->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL) {
        /* A physical pool is storage only; its pages become GPU-visible through the virtual pools they are mapped into. */
        if (!s->mem || !s->size) { LOGE("physical MemoryPool without storage (flags=%#x size=%#llx)", s->flags, (unsigned long long)s->size); return 0; }
        o->magic = POOL_MAGIC; o->flags = s->flags; o->mem = s->mem; o->size = s->size;
        g_last_phys_mis = (uint64_t)(uintptr_t)s->mem;
        ap_add(o);
        LOGD("physical MemoryPool %p mem=%p size=%#llx flags=%#x", (void *)p, (void *)o->mem, (unsigned long long)o->size, o->flags);
        return 1;
    }
    if (!s->mem || !s->size) { LOGE("MemoryPoolInitialize without storage (flags=%#x size=%#llx)", s->flags, (unsigned long long)s->size); return 0; }
    o->magic = POOL_MAGIC;
    o->flags = s->flags;
    o->mem = s->mem;
    o->size = s->size;
    if (!(o->flags & NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS)) {
        o->mtl = gfx_mtl_pool_add(o->mem, o->size, o->flags);
        if (o->mtl) dirty_add_range(o->mem, o->size);          /* removed in Finalize under the same condition */
    }
    if (o->flags & 0x40) cp_add(o);
    ap_add(o);
    LOGD("MemoryPool %p mem=%p size=%#llx flags=%#x", (void *)p, (void *)o->mem, (unsigned long long)o->size, o->flags);
    return 1;
}
NVN(nvnMemoryPoolSetDebugLabel, void, NVNmemoryPool *p, const char *l) { (void)p; (void)l; }
/* ---- virtual pools ------------------------------------------------------------------------------------------ */
typedef struct vmap { uint64_t voff, size; void *mtl; } vmap;
/* NVNmappingRequest (not defined by the public header): { NVNmemoryPool *physicalPool; int64 physicalOffset; int64 virtualOffset; int64 size; int storageClass; } */
typedef struct { pool_t *phys; int64_t poff, voff, size; int32_t storage_class; int32_t pad; } mapreq_t;

static void vmap_drop(vmap *m, pool_t *vp) {
    if (!m->mtl) return;
    dirty_remove_range(vp->mem + m->voff, (size_t)m->size);
    gfx_mtl_pool_remove(m->mtl);
    m->mtl = NULL;
}
/* Remove [voff, voff+size) from the mapped list (whole records; partial overlaps shrink the record by re-registering the remainder). */
static void vmap_cut(pool_t *vp, uint64_t voff, uint64_t size) {
    for (uint32_t i = 0; i < vp->nmaps;) {
        vmap m = vp->maps[i];
        uint64_t lo = m.voff, hi = m.voff + m.size;
        if (hi <= voff || lo >= voff + size) { i++; continue; }
        vmap_drop(&vp->maps[i], vp);
        vp->maps[i] = vp->maps[--vp->nmaps];
        /* keep the parts of the old range outside the cut: their pages are still mapped and must stay reachable */
        if (lo < voff) { vmap *n = &vp->maps[vp->nmaps++]; *n = (vmap){ lo, voff - lo, NULL }; n->mtl = gfx_mtl_pool_add(vp->mem + lo, voff - lo, vp->flags); if (n->mtl) dirty_add_range(vp->mem + lo, voff - lo); }
        if (hi > voff + size) { vmap *n = &vp->maps[vp->nmaps++]; *n = (vmap){ voff + size, hi - (voff + size), NULL }; n->mtl = gfx_mtl_pool_add(vp->mem + n->voff, n->size, vp->flags); if (n->mtl) dirty_add_range(vp->mem + n->voff, n->size); }
        i = 0;     /* the list changed: rescan */
    }
}

NVN(nvnMemoryPoolMapVirtual, NVNboolean, NVNmemoryPool *p, int n, const NVNmappingRequest *reqs) {
    pool_t *vp = (pool_t *)p;
    if (vp->magic != POOL_MAGIC || !(vp->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL)) { LOGE("MapVirtual on a pool that is not virtual"); return 0; }
    const mapreq_t *r = (const mapreq_t *)reqs;
    uintptr_t pg = (uintptr_t)getpagesize();
    for (int i = 0; i < n; i++) {
        static int dumped;
        if (dumped < 6) { dumped++; LOGD("MapVirtual request %d: phys %p poff %#llx voff %#llx size %#llx class %d", i, (void *)r[i].phys, (long long)r[i].poff, (long long)r[i].voff, (long long)r[i].size, r[i].storage_class); }
        if (r[i].voff < 0 || r[i].size <= 0 || (uint64_t)r[i].voff + (uint64_t)r[i].size > vp->size) { LOGE("MapVirtual: range outside the pool"); return 0; }
        uint64_t voff = (uint64_t)r[i].voff, size = (uint64_t)r[i].size;
        uint8_t *dst = vp->mem + voff;
        vmap_cut(vp, voff, size);
        pool_t *pp = r[i].phys;
        if (!pp || pp->magic != POOL_MAGIC || !(pp->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL)) {
            /* unmap: the range reads as zero again */
            uintptr_t ulo = ((uintptr_t)dst + pg - 1) & ~(pg - 1), uhi = ((uintptr_t)dst + size) & ~(pg - 1);      /* only whole host pages inside the range */
            if (uhi > ulo && mmap((void *)ulo, uhi - ulo, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) == MAP_FAILED) { LOGE("MapVirtual: unmap failed"); return 0; }
            if (uhi > ulo) dirty_invalidate((void *)ulo, uhi - ulo);
            continue;
        }
        if (r[i].poff < 0 || (uint64_t)r[i].poff + size > pp->size) { LOGE("MapVirtual: physical range outside its pool"); return 0; }
        uint8_t *src = pp->mem + r[i].poff;
        { static int ml = -1; if (ml < 0) ml = getenv("NAPPLE_MAPLOG") != NULL; if (ml) fprintf(stderr, "[maplog] virtual %p..%p <- physical %p (pool %p off %#llx size %#llx)\n", (void *)dst, (void *)(dst + size), (void *)src, (void *)pp, (unsigned long long)r[i].poff, (unsigned long long)size); }
        if (((uintptr_t)dst - (uintptr_t)src) & (pg - 1)) { LOG_ONCE(NAPPLE_LOG_ERROR, "MapVirtual: virtual %p and physical %p ranges differ modulo the host page size: cannot alias", (void *)dst, (void *)src); return 0; }
        uintptr_t dlo = (uintptr_t)dst & ~(pg - 1), dhi = ((uintptr_t)dst + size + pg - 1) & ~(pg - 1), slo = (uintptr_t)src & ~(pg - 1);
        mach_vm_address_t a = (mach_vm_address_t)dlo;
        vm_prot_t cur, max;
        kern_return_t kr = mach_vm_remap(mach_task_self(), &a, dhi - dlo, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (mach_vm_address_t)slo, FALSE, &cur, &max, VM_INHERIT_NONE);
        if (kr == KERN_SUCCESS) dirty_invalidate((void *)dlo, dhi - dlo);      /* the new pages are writable: neighbours sharing a boundary page lose their protection */
        if (kr != KERN_SUCCESS) { LOGE("MapVirtual: mach_vm_remap failed (%d) for %p+%#llx -> %p", kr, (void *)src, (unsigned long long)size, (void *)dst); return 0; }
        if (vp->nmaps == vp->capmaps) { vp->capmaps = vp->capmaps ? vp->capmaps * 2 : 16; vp->maps = realloc(vp->maps, vp->capmaps * sizeof(vmap)); }
        vmap *m = &vp->maps[vp->nmaps++];
        *m = (vmap){ voff, size, gfx_mtl_pool_add(dst, size, vp->flags) };
        if (m->mtl) dirty_add_range(dst, size);
    }
    return 1;
}

NVN(nvnMemoryPoolFinalize, void, NVNmemoryPool *p) {
    pool_t *o = (pool_t *)p;
    if (o->magic != POOL_MAGIC) return;
    if (o->flags & 0x40) cp_remove(o);
    ap_remove(o);
    if (o->mem) gfx_cmdrec_pool_freed(o->mem, (size_t)o->size);      /* command lists recorded into this memory die with it */
    if (o->mtl) { dirty_remove_range(o->mem, (size_t)o->size); gfx_mtl_pool_remove(o->mtl); }
    if (o->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) {
        for (uint32_t i = 0; i < o->nmaps; i++) vmap_drop(&o->maps[i], o);
        free(o->maps);
        munmap(o->vraw, (size_t)o->vraw_size);      /* every remapped range is part of the reservation */
    }
    o->magic = 0;
    o->mtl = NULL;
    o->maps = NULL; o->nmaps = o->capmaps = 0;
}
NVN(nvnMemoryPoolMap, void *, const NVNmemoryPool *p) {
    const pool_t *o = (const pool_t *)p;
    { static int pl = -1; if (pl < 0) pl = getenv("NAPPLE_POOLLOG") != NULL; if (pl) fprintf(stderr, "[poollog] Map pool %p flags %#x mem %p\n", (const void *)p, o->flags, (void *)o->mem); }
    if (o->magic != POOL_MAGIC || (o->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) return NULL;
    return o->mem;
}
/* CPU and GPU share memory and caches on Apple silicon: flush and invalidate have nothing to do. */
NVN(nvnMemoryPoolFlushMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t n) {
    static int pl = -1; if (pl < 0) pl = getenv("NAPPLE_POOLLOG") != NULL;
    if (pl) fprintf(stderr, "[poollog] Flush pool %p flags %#x off %#llx n %#llx\n", (const void *)p, ((const pool_t *)p)->flags, (unsigned long long)off, (unsigned long long)n);
}
NVN(nvnMemoryPoolInvalidateMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t n) { (void)p; (void)off; (void)n; }
NVN(nvnMemoryPoolGetBufferAddress, NVNbufferAddress, const NVNmemoryPool *p) { const pool_t *o = (const pool_t *)p;
    { static int pl = -1; if (pl < 0) pl = getenv("NAPPLE_POOLLOG") != NULL; if (pl) fprintf(stderr, "[poollog] GetBufferAddress pool %p magic %#x flags %#x mem %p size %#llx\n", (const void *)p, o->magic, o->flags, (void *)o->mem, (unsigned long long)o->size); } if (!o->mem) LOGW("MemoryPoolGetBufferAddress: pool %p has no storage (magic %#x flags %#x size %#llx)", (const void *)p, o->magic, o->flags, (unsigned long long)o->size); return (NVNbufferAddress)(uintptr_t)o->mem; }
NVN(nvnMemoryPoolGetSize, size_t, const NVNmemoryPool *p) { if (getenv("NAPPLE_POOLLOG")) fprintf(stderr, "[poollog] GetSize pool %p flags %#x\n", (const void *)p, ((const pool_t *)p)->flags); return (size_t)((const pool_t *)p)->size; }
NVN(nvnMemoryPoolGetFlags, NVNmemoryPoolFlags, const NVNmemoryPool *p) { if (getenv("NAPPLE_POOLLOG")) fprintf(stderr, "[poollog] GetFlags pool %p\n", (const void *)p); return (NVNmemoryPoolFlags)((const pool_t *)p)->flags; }

/* ---------------------------------------------------------------------------------------------------------------
 * Buffers
 * --------------------------------------------------------------------------------------------------------------- */
#define BB(b) ((buf_builder_t *)(b))
NVN(nvnBufferBuilderSetDevice, void, NVNbufferBuilder *b, NVNdevice *d) { BB(b)->dev = d; }
NVN(nvnBufferBuilderSetDefaults, void, NVNbufferBuilder *b) { NVNdevice *d = BB(b)->dev; memset(b, 0, sizeof(buf_builder_t)); BB(b)->dev = d; }
NVN(nvnBufferBuilderSetStorage, void, NVNbufferBuilder *b, NVNmemoryPool *p, ptrdiff_t off, size_t size) { BB(b)->pool = (pool_t *)p; BB(b)->off = off; BB(b)->size = size; }
NVN(nvnBufferBuilderGetDevice, const NVNdevice *, const NVNbufferBuilder *b) { return BB(b)->dev; }
NVN(nvnBufferBuilderGetMemoryPool, NVNmemoryPool *, const NVNbufferBuilder *b) { return (NVNmemoryPool *)BB(b)->pool; }
NVN(nvnBufferBuilderGetMemoryOffset, ptrdiff_t, const NVNbufferBuilder *b) { return (ptrdiff_t)BB(b)->off; }
NVN(nvnBufferBuilderGetSize, size_t, const NVNbufferBuilder *b) { return (size_t)BB(b)->size; }

NVN(nvnBufferInitialize, NVNboolean, NVNbuffer *b, const NVNbufferBuilder *builder) {
    const buf_builder_t *v = (const buf_builder_t *)builder;
    if (!v->pool || v->pool->magic != POOL_MAGIC || v->off < 0 || (uint64_t)v->off > v->pool->size || v->size > v->pool->size - (uint64_t)v->off) {
        LOGE("BufferInitialize: invalid storage (pool %p off %lld size %llu)", (void *)v->pool, (long long)v->off, (unsigned long long)v->size);
        return 0;
    }
    *(buf_t *)b = (buf_t){ BUF_MAGIC, v->pool, v->off, v->size };
    return 1;
}
NVN(nvnBufferSetDebugLabel, void, NVNbuffer *b, const char *l) { (void)b; (void)l; }
NVN(nvnBufferFinalize, void, NVNbuffer *b) { memset(b, 0, sizeof(buf_t)); }
NVN(nvnBufferMap, void *, const NVNbuffer *b) {
    const buf_t *v = (const buf_t *)b;
    if (v->magic != BUF_MAGIC || (v->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) return NULL;
    return v->pool->mem + v->off;
}
NVN(nvnBufferGetAddress, NVNbufferAddress, const NVNbuffer *b) { const buf_t *v = (const buf_t *)b;
    { static int pl = -1; if (pl < 0) pl = getenv("NAPPLE_POOLLOG") != NULL; if (pl) fprintf(stderr, "[poollog] BufferGetAddress buf %p magic %#x pool %p flags %#x off %#llx size %#llx\n", (const void *)b, v->magic, (const void *)v->pool, v->pool ? v->pool->flags : 0, (unsigned long long)v->off, (unsigned long long)v->size); } if (!v->pool || !v->pool->mem) LOGW("BufferGetAddress: buffer %p has no pool storage (pool %p off %#llx)", (const void *)b, (const void *)v->pool, (unsigned long long)v->off); return (NVNbufferAddress)(uintptr_t)(v->pool->mem + v->off); }
NVN(nvnBufferFlushMappedRange, void, const NVNbuffer *b, ptrdiff_t off, size_t size) { (void)b; (void)off; (void)size; }
NVN(nvnBufferInvalidateMappedRange, void, const NVNbuffer *b, ptrdiff_t off, size_t size) { (void)b; (void)off; (void)size; }
NVN(nvnBufferGetMemoryPool, NVNmemoryPool *, const NVNbuffer *b) { return (NVNmemoryPool *)((const buf_t *)b)->pool; }
NVN(nvnBufferGetMemoryOffset, ptrdiff_t, const NVNbuffer *b) { return (ptrdiff_t)((const buf_t *)b)->off; }
NVN(nvnBufferGetSize, size_t, const NVNbuffer *b) { return (size_t)((const buf_t *)b)->size; }
NVN(nvnBufferGetDebugID, uint64_t, const NVNbuffer *b) { return (uint64_t)(uintptr_t)b; }
