#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
/* api_program.c - NVN programs: a program is a set of shader blobs (one per stage) identified by content hash. */
#include "gfx_mtl.h"

void gfx_api_link_program(void) {}

#define PROG_MAGIC MAGIC('P', 'R', 'G', '0')

/* Stage hashes of each program, also kept host-side by program address. prog_t (152 bytes) assumes the guest's NVNprogram is that large; a title whose SDK
 * declares it smaller has its next object over the tail of prog_t (hash[] starts at +0x68), and the guest's writes there show up as bogus stage hashes at
 * BindProgram (Animal Crossing: "program hash 0x3"). BindProgram takes the hashes from here and reports such an overwrite once (gfx_program_hashes). */
/* Animal Crossing's neighbour object starts at +0x88 at the latest: nothing is written into the guest object from there on (hash[4] / hash[5], tess eval and
 * compute, live only host-side; GUEST_HASHES stages are mirrored in the object for programs the guest copies by value). */
#define GUEST_BYTES (offsetof(prog_t, hash) + 4 * sizeof(uint64_t))
#define GUEST_HASHES 4
_Static_assert(offsetof(prog_t, hash) + GUEST_HASHES * sizeof(uint64_t) <= 0x88, "prog_t's guest-visible part must stay below +0x88");
#define PH_N 16384
static struct { const void *p; uint64_t hash[6]; } g_ph[PH_N];
static pthread_mutex_t g_ph_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t ph_slot(const void *p) { return (uint32_t)(((uintptr_t)p >> 4) * 2654435761u) % PH_N; }
static void ph_set(const void *p, const uint64_t *h) {
    uint32_t i = ph_slot(p);
    pthread_mutex_lock(&g_ph_mu);
    g_ph[i].p = h ? p : NULL;
    if (h) memcpy(g_ph[i].hash, h, sizeof g_ph[i].hash);
    pthread_mutex_unlock(&g_ph_mu);
}
static int ph_get(const void *p, uint64_t out[6]) {
    uint32_t i = ph_slot(p);
    int ok = 0;
    pthread_mutex_lock(&g_ph_mu);
    if (g_ph[i].p == p) { memcpy(out, g_ph[i].hash, sizeof g_ph[i].hash); ok = 1; }
    pthread_mutex_unlock(&g_ph_mu);
    return ok;
}
/* The stage hashes of a program for BindProgram and the per-program state setters (api_state.c): the host copy; for an address the table does not know (a
 * program object the guest copied by value, an evicted slot) the guest object's, with values that cannot be shader hashes (small integers: guest data over
 * the tail of prog_t) dropped. Returns 0 when `p` is not an initialized program. */
int gfx_program_hashes(const NVNprogram *p, uint64_t out[6]) {
    const prog_t *o = (const prog_t *)p;
    memset(out, 0, 6 * sizeof *out);
    if (!o || o->magic != PROG_MAGIC) return 0;
    if (ph_get(p, out)) {
        if (memcmp(out, o->hash, GUEST_HASHES * sizeof *out)) {
            int k = 0;
            while (k < GUEST_HASHES && out[k] == o->hash[k]) k++;
            LOG_ONCE(NAPPLE_LOG_INFO, "program %p: stage hash %d (object offset %#zx) holds %#llx, SetShaders stored %#llx: the guest object is smaller than prog_t "
                     "(host copy used)", (const void *)p, k, offsetof(prog_t, hash) + 8 * (size_t)k, (unsigned long long)o->hash[k], (unsigned long long)out[k]);
        }
        return 1;
    }
    memcpy(out, o->hash, GUEST_HASHES * sizeof *out);
    for (int k = 0; k < GUEST_HASHES; k++)
        if (out[k] && out[k] < 0x100000) {
            LOG_ONCE(NAPPLE_LOG_INFO, "program %p (not set up at this address: copied by value?): stage hash %d holds %#llx, dropped", (const void *)p, k,
                     (unsigned long long)out[k]);
            out[k] = 0;
        }
    return 1;
}

NVN(nvnProgramInitialize, NVNboolean, NVNprogram *p, NVNdevice *d) {
    (void)d;
    prog_t *o = (prog_t *)p;
    memset(o, 0, GUEST_BYTES);
    o->magic = PROG_MAGIC;
    static const uint64_t none[6];
    ph_set(p, none);
    return 1;
}
NVN(nvnProgramFinalize, void, NVNprogram *p) { memset(p, 0, GUEST_BYTES); ph_set(p, NULL); }

/* ProgramSetShaders may name a stage's code by a bare offset into a shader-code pool (Animal Crossing sets its window-pass program, and others, this way: the
 * address it adds the offset to is 0). The game checks the result and, when the call fails, never binds the program: the window pass then drew with the previous
 * draw's program, constants and texture (the "orphan composite" of the old notes). The call succeeds and each such stage gets a placeholder hash (high word "PLCH");
 * the replay resolves it at the first draw, when the game has filled the pools: the oldest shader-code pool that has an NVN shader header (magic 0x12345678) at the
 * offsets of ALL stages of that call (they come from one shader archive). NAPPLE_PROGRAM_POOL_OFFSETS=0 restores the rejection. */
const uint8_t *gfx_find_code_pool(uint64_t off, uint64_t need, uint64_t after);
const uint8_t *gfx_find_code_pool_multi(const uint64_t *offs, const uint64_t *need, const int *stage, int n);
uint64_t gfx_code_pool_seq(void);
#define STUB_TAG 0x504c434800000000ull   /* "PLCH" in the high word: a placeholder, never a content hash in practice (2^-32) */
#define IS_STUB(h) (((h) >> 32) == 0x504c4348ull)
typedef struct { uint64_t off, pseq, real; int stage; uint32_t grp; uint8_t ctl[0x880]; } stub_t;
static stub_t **g_stubs; static uint32_t g_nstubs, g_capstubs;
static pthread_mutex_t g_stub_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t stub_new(uint64_t off, int stage, const uint8_t *control, uint32_t grp) {
    pthread_mutex_lock(&g_stub_mu);
    if (g_nstubs == g_capstubs) { g_capstubs = g_capstubs ? g_capstubs * 2 : 64; g_stubs = realloc(g_stubs, g_capstubs * sizeof *g_stubs); }
    stub_t *t = calloc(1, sizeof *t);
    t->off = off; t->stage = stage; t->grp = grp; t->pseq = gfx_code_pool_seq(); memcpy(t->ctl, control, sizeof t->ctl);
    g_stubs[g_nstubs] = t;
    uint64_t id = STUB_TAG | g_nstubs++;
    pthread_mutex_unlock(&g_stub_mu);
    return id;
}
uint64_t gfx_prog_resolve_hash(uint64_t h) {
    if (h && h < 0x100000) { LOG_ONCE(NAPPLE_LOG_WARN, "program hash %#llx is not a shader hash (a guest write into the program object?): ignored", (unsigned long long)h); return 0; }
    if (!IS_STUB(h)) return h;
    uint64_t real = 0;
    pthread_mutex_lock(&g_stub_mu);
    if ((uint32_t)h < g_nstubs) {
        stub_t *t = g_stubs[(uint32_t)h];
        if (!t->real) {
            uint32_t const_off, const_size;
            memcpy(&const_off, t->ctl + 0x700, 4); memcpy(&const_size, t->ctl + 0x6FC, 4);
            const uint8_t *code = NULL;
            {
                uint64_t offs[6], need[6]; int stg[6], n = 0;
                for (uint32_t i = 0; i < g_nstubs && n < 6; i++)
                    if (g_stubs[i]->grp == t->grp) { uint32_t co, cs; memcpy(&co, g_stubs[i]->ctl + 0x700, 4); memcpy(&cs, g_stubs[i]->ctl + 0x6FC, 4); offs[n] = g_stubs[i]->off; stg[n] = g_stubs[i]->stage; need[n++] = (uint64_t)co + cs; }
                const uint8_t *base = gfx_find_code_pool_multi(offs, need, stg, n);
                if (base) code = base + t->off;
            }
            if (code) {
                t->real = gfx_shader_register(t->stage, (uint64_t)(uintptr_t)code, t->ctl);
                { const uint32_t *w = (const uint32_t *)code; LOGD("  code words %08x ... sph %08x %08x %08x %08x stage %d codelen %#x constlen %#x", w[0], w[12], w[13], w[14], w[15], t->stage, const_off, const_size); }
                LOGD("program stage %d: code at pool offset %#llx resolved to %p (%016llx)", t->stage, (unsigned long long)t->off, (const void *)code, (unsigned long long)t->real);
            } else LOG_ONCE(NAPPLE_LOG_WARN, "program stage %d: code at pool offset %#llx not found in a shader code pool created after the call", t->stage, (unsigned long long)t->off);
        }
        real = t->real;
    }
    pthread_mutex_unlock(&g_stub_mu);
    { static _Atomic uint64_t nz; if (!real && IS_STUB(h) && getenv("NAPPLE_POOLLOG") && (atomic_fetch_add(&nz, 1) & 1023) == 0) fprintf(stderr, "[stub] placeholder %#llx unresolved (%llu)\n", (unsigned long long)h, (unsigned long long)nz); }
    return real;
}

static NVNboolean set_shaders(NVNprogram *p, int n, const uint8_t *data, size_t stride) {
    prog_t *o = (prog_t *)p;
    if (o->magic != PROG_MAGIC || n < 0 || n > 6) return 0;
    uint64_t hs[6] = {0};                                 /* stage hashes: host table + the guest-visible GUEST_HASHES */
    memset(o->hash, 0, GUEST_HASHES * sizeof *hs);
    ph_set(p, hs);
    o->nstage = 0;
    static _Atomic uint32_t g_grp;
    uint32_t grp = atomic_fetch_add(&g_grp, 1) + 1;     /* the stubs of one call belong together */
    for (int i = 0; i < n; i++) {
        uint64_t code = *(const uint64_t *)(data + (size_t)i * stride);
        const uint8_t *control = (const uint8_t *)(uintptr_t) * (const uint64_t *)(data + (size_t)i * stride + 8);
        if (!control) { LOGE("ProgramSetShaders: null control section"); return 0; }
        if ((uint64_t)(uintptr_t)control < 0x100000000ull) { LOGW("ProgramSetShaders: stage %d control section %p is not a mapped address; program rejected", i, (const void *)control); return 0; }
        static int accept_off = -1; if (accept_off < 0) { const char *e = getenv("NAPPLE_PROGRAM_POOL_OFFSETS"); accept_off = !(e && e[0] == '0'); }
        if (code < 0x100000000ull && accept_off) {   /* every host pointer is above the 4 GiB page-zero of an arm64 Mac process: a pool offset */
            int stage = (int)*(const uint32_t *)(control + 0x714);
            if (stage < 0 || stage > 5 || i >= 6) { LOGE("ProgramSetShaders: control section names stage %d", stage); return 0; }
            hs[stage] = stub_new(code, stage, control, grp);
            o->nstage++;
            continue;
        }
        if (code < 0x100000000ull) {
            static _Atomic uint64_t n_bad;
            uint64_t k = atomic_fetch_add(&n_bad, 1);
            if (k < 8 || (k & (k - 1)) == 0) LOGW("ProgramSetShaders: stage %d code address %#llx / control %p is not a mapped address (%llu so far); program rejected (NAPPLE_PROGRAM_POOL_OFFSETS=0)", i, (unsigned long long)code, (const void *)control, (unsigned long long)k + 1);
            return 0;
        }
        if (i < 6) { o->sd[i].data = code; o->sd[i].control = control; }
        /* The stage comes from the control section (what the shader compiler recorded); gfx_shader_register validates it. */
        int stage = (int)*(const uint32_t *)(control + 0x714);
        if (stage < 0 || stage > 5) { LOGE("ProgramSetShaders: control section names stage %d", stage); return 0; }
        { static int k; if (k++ < 4) { const uint32_t *w = (const uint32_t *)(uintptr_t)code; LOGD("normal code %#llx words %08x %08x %08x %08x %08x %08x %08x %08x (stage %d codelen %#x)", (unsigned long long)code, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], stage, *(const uint32_t *)(control + 0x700)); } }
        uint64_t h = gfx_shader_register(stage, code, control);
        if (!h) return 0;
        hs[stage] = h;
        o->nstage++;
    }
    memcpy(o->hash, hs, GUEST_HASHES * sizeof *hs);
    ph_set(p, hs);
    return 1;
}
NVN(nvnProgramSetShaders, NVNboolean, NVNprogram *p, int n, const NVNshaderData *d) { return set_shaders(p, n, (const uint8_t *)d, sizeof(NVNshaderData)); }
/* NVNshaderDataExt = { data, control, reserved }, 0x18 bytes. */
NVN(nvnProgramSetShadersExt, NVNboolean, NVNprogram *p, int n, const void *d) { return set_shaders(p, n, (const uint8_t *)d, 24); }
NVN(nvnProgramSetSubroutineLinkage, NVNboolean, NVNprogram *p, int n, const NVNsubroutineLinkageMapPtr *m) { (void)p; (void)n; (void)m; return 1; }
/* Sample shading: kept in a side table keyed by the program address, NOT in the program object. The guest's NVNprogram may be smaller than the 0xC0 bytes of this
 * header (the SDK it was built with decides; writing past prog_t's 152 bytes crashed Animal Crossing at boot by corrupting a neighbour). Metal has no pipeline switch
 * for it: a fragment function runs once per sample exactly when it reads [[sample_id]] or a per-sample interpolant, a property of the translated shader. BindProgram
 * records the flag (p_bind_program.sample_shading, api_cmd.c), the replay puts it in the pipeline key of multisampled passes and the fragment function is translated
 * with [[sample_id]] (ShaderKey.sample_shading). Any rate > 0 shades every sample (Metal has no minimum sample fraction). The debug label is accepted and dropped. */
#define SS_N 4096
static struct { const void *p; uint32_t en; float rate; } g_ss[SS_N];
static pthread_mutex_t g_ss_mu = PTHREAD_MUTEX_INITIALIZER;
NVN(nvnProgramSetSampleShading, void, NVNprogram *p, uint32_t enable, float rate) {
    uint32_t i = (uint32_t)(((uintptr_t)p >> 4) * 2654435761u) % SS_N;
    pthread_mutex_lock(&g_ss_mu);
    g_ss[i].p = p;
    g_ss[i].en = enable ? 1 : 0;
    g_ss[i].rate = rate < 0 ? 0 : (rate > 1 ? 1 : rate);
    pthread_mutex_unlock(&g_ss_mu);
}
NVN(nvnProgramSetDebugLabel, void, NVNprogram *p, const char *l) { (void)p; (void)l; }
/* Subtile (tiled cache) size the program asks for: none here (0 x 0 = the driver's choice); Metal's tile renderer picks its own. */
NVN(nvnProgramGetSubtileSize, void, const NVNprogram *p, int *w, int *h) {
    (void)p;
    if (w) *w = 0;
    if (h) *h = 0;
}
/* test hook: the stored sample shading (enable, rate) */
int gfx_program_sample_shading(const NVNprogram *p, float *rate) {
    uint32_t i = (uint32_t)(((uintptr_t)p >> 4) * 2654435761u) % SS_N;
    int en = 0;
    float r = 0;
    pthread_mutex_lock(&g_ss_mu);
    if (g_ss[i].p == p) { en = (int)g_ss[i].en; r = g_ss[i].rate; }
    pthread_mutex_unlock(&g_ss_mu);
    if (rate) *rate = r;
    return en;
}
