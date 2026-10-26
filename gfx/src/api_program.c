/* api_program.c - NVN programs: a program is a set of shader blobs (one per stage) identified by content hash. */
#include "gfx_mtl.h"

void gfx_api_link_program(void) {}

#define PROG_MAGIC MAGIC('P', 'R', 'G', '0')

NVN(nvnProgramInitialize, NVNboolean, NVNprogram *p, NVNdevice *d) {
    (void)d;
    prog_t *o = (prog_t *)p;
    memset(o, 0, sizeof *o);
    o->magic = PROG_MAGIC;
    return 1;
}
NVN(nvnProgramFinalize, void, NVNprogram *p) { memset(p, 0, sizeof(prog_t)); }
NVN(nvnProgramSetDebugLabel, void, NVNprogram *p, const char *l) { (void)p; (void)l; }

static NVNboolean set_shaders(NVNprogram *p, int n, const uint8_t *data, size_t stride) {
    prog_t *o = (prog_t *)p;
    if (o->magic != PROG_MAGIC || n < 0 || n > 6) return 0;
    memset(o->hash, 0, sizeof o->hash);
    o->nstage = 0;
    for (int i = 0; i < n; i++) {
        uint64_t code = *(const uint64_t *)(data + (size_t)i * stride);
        const uint8_t *control = (const uint8_t *)(uintptr_t) * (const uint64_t *)(data + (size_t)i * stride + 8);
        if (!code || !control) { LOGE("ProgramSetShaders: null code or control section"); return 0; }
        if (code < 0x100000000ull || (uint64_t)(uintptr_t)control < 0x100000000ull) {   /* every host pointer is above the 4 GiB page-zero of an arm64 Mac process */      /* a pool-relative offset, not an address: the pool base the game added was 0 */
            static _Atomic uint64_t n_bad;
            uint64_t k = atomic_fetch_add(&n_bad, 1);
            if (k < 8 || (k & (k - 1)) == 0) LOGW("ProgramSetShaders: stage %d code address %#llx / control %p is not a mapped address (%llu so far); program rejected", i, (unsigned long long)code, (const void *)control, (unsigned long long)k + 1);
            return 0;
        }
        if (i < 6) { o->sd[i].data = code; o->sd[i].control = control; }
        /* The stage comes from the control section (what the shader compiler recorded); gfx_shader_register validates it. */
        int stage = (int)*(const uint32_t *)(control + 0x714);
        if (stage < 0 || stage > 5) { LOGE("ProgramSetShaders: control section names stage %d", stage); return 0; }
        uint64_t h = gfx_shader_register(stage, code, control);
        if (!h) return 0;
        o->hash[stage] = h;
        o->nstage++;
    }
    return 1;
}
NVN(nvnProgramSetShaders, NVNboolean, NVNprogram *p, int n, const NVNshaderData *d) { return set_shaders(p, n, (const uint8_t *)d, sizeof(NVNshaderData)); }
/* NVNshaderDataExt = { data, control, reserved }, 0x18 bytes. */
NVN(nvnProgramSetShadersExt, NVNboolean, NVNprogram *p, int n, const void *d) { return set_shaders(p, n, (const uint8_t *)d, 24); }
NVN(nvnProgramSetSubroutineLinkage, NVNboolean, NVNprogram *p, int n, const NVNsubroutineLinkageMapPtr *m) { (void)p; (void)n; (void)m; return 1; }
NVN(nvnProgramSetSampleShading, void, NVNprogram *p, uint32_t enable, float rate) { (void)p; (void)enable; (void)rate; }
