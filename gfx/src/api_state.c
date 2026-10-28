/* api_state.c - NVN state objects (blend, color, channel mask, multisample, polygon, depth/stencil, vertex attrib/stream).
 * They are small value types the game copies freely, so they are packed in place (obj.h) and bound by value. */
#include "obj.h"

void gfx_api_link_state(void) {}

#define ST(T, x) ((T *)(x))
#define CST(T, x) ((const T *)(x))

/* ---- blend ---- */
NVN(nvnBlendStateSetDefaults, void, NVNblendState *s) {
    *ST(blend_st, s) = (blend_st){ 0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD };
}
NVN(nvnBlendStateSetBlendTarget, void, NVNblendState *s, int t) { ST(blend_st, s)->target = (uint64_t)t; }
NVN(nvnBlendStateSetBlendFunc, void, NVNblendState *s, NVNblendFunc sr, NVNblendFunc dr, NVNblendFunc sa, NVNblendFunc da) {
    blend_st *o = ST(blend_st, s); o->src_rgb = sr; o->dst_rgb = dr; o->src_a = sa; o->dst_a = da;
}
NVN(nvnBlendStateSetBlendEquation, void, NVNblendState *s, NVNblendEquation er, NVNblendEquation ea) { ST(blend_st, s)->eq_rgb = er; ST(blend_st, s)->eq_a = ea; }
NVN(nvnBlendStateSetAdvancedMode, void, NVNblendState *s, int m) { ST(blend_st, s)->adv_mode = (uint64_t)m & 0x3f; }
NVN(nvnBlendStateSetAdvancedOverlap, void, NVNblendState *s, int m) { ST(blend_st, s)->adv_overlap = (uint64_t)m & 3; }
NVN(nvnBlendStateSetAdvancedPremultipliedSrc, void, NVNblendState *s, uint32_t e) { ST(blend_st, s)->adv_nonpremul = !B(e); }
NVN(nvnBlendStateSetAdvancedNormalizedDst, void, NVNblendState *s, uint32_t e) { ST(blend_st, s)->adv_nonnorm = !B(e); }
NVN(nvnBlendStateGetBlendTarget, int, const NVNblendState *s) { return (int)CST(blend_st, s)->target; }
NVN(nvnBlendStateGetBlendFunc, void, const NVNblendState *s, int *sr, int *dr, int *sa, int *da) {
    const blend_st *o = CST(blend_st, s); *sr = (int)o->src_rgb; *dr = (int)o->dst_rgb; *sa = (int)o->src_a; *da = (int)o->dst_a;
}
NVN(nvnBlendStateGetBlendEquation, void, const NVNblendState *s, int *er, int *ea) { *er = (int)CST(blend_st, s)->eq_rgb; *ea = (int)CST(blend_st, s)->eq_a; }
NVN(nvnBlendStateGetAdvancedMode, int, const NVNblendState *s) { return (int)CST(blend_st, s)->adv_mode; }
NVN(nvnBlendStateGetAdvancedOverlap, int, const NVNblendState *s) { return (int)CST(blend_st, s)->adv_overlap; }
NVN(nvnBlendStateGetAdvancedPremultipliedSrc, NVNboolean, const NVNblendState *s) { return !CST(blend_st, s)->adv_nonpremul; }
NVN(nvnBlendStateGetAdvancedNormalizedDst, NVNboolean, const NVNblendState *s) { return !CST(blend_st, s)->adv_nonnorm; }

/* ---- color ---- */
NVN(nvnColorStateSetDefaults, void, NVNcolorState *s) { *ST(color_st, s) = (color_st){ 0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS }; }
NVN(nvnColorStateSetBlendEnable, void, NVNcolorState *s, int i, uint32_t en) {
    color_st *o = ST(color_st, s);
    if (i < 0 || i > 7) return;
    o->blend_en = B(en) ? (o->blend_en | 1u << i) : (o->blend_en & ~(1u << i));
}
NVN(nvnColorStateSetLogicOp, void, NVNcolorState *s, int op) { ST(color_st, s)->logic_op = (uint32_t)op; }
NVN(nvnColorStateSetAlphaTest, void, NVNcolorState *s, int f) { ST(color_st, s)->alpha_func = (uint32_t)f; }
NVN(nvnColorStateGetBlendEnable, NVNboolean, const NVNcolorState *s, int i) { return (i >= 0 && i < 8) ? (CST(color_st, s)->blend_en >> i) & 1 : 0; }
NVN(nvnColorStateGetLogicOp, int, const NVNcolorState *s) { return (int)CST(color_st, s)->logic_op; }
NVN(nvnColorStateGetAlphaTest, int, const NVNcolorState *s) { return (int)CST(color_st, s)->alpha_func; }

/* ---- channel mask ---- */
NVN(nvnChannelMaskStateSetDefaults, void, NVNchannelMaskState *s) { ST(chmask_st, s)->mask = 0xffffffffu; }
NVN(nvnChannelMaskStateSetChannelMask, void, NVNchannelMaskState *s, int i, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (i < 0 || i > 7) return;
    uint32_t m = (B(r) ? 1u : 0u) | (B(g) ? 2u : 0u) | (B(b) ? 4u : 0u) | (B(a) ? 8u : 0u);
    chmask_st *o = ST(chmask_st, s);
    o->mask = (o->mask & ~(0xfu << (4 * i))) | m << (4 * i);
}
NVN(nvnChannelMaskStateGetChannelMask, void, const NVNchannelMaskState *s, int i, NVNboolean *r, NVNboolean *g, NVNboolean *b, NVNboolean *a) {
    uint32_t m = (i >= 0 && i < 8) ? (CST(chmask_st, s)->mask >> (4 * i)) & 0xf : 0;
    *r = m & 1; *g = (m >> 1) & 1; *b = (m >> 2) & 1; *a = (m >> 3) & 1;
}

/* ---- multisample ---- */
NVN(nvnMultisampleStateSetDefaults, void, NVNmultisampleState *s) { memset(s, 0, sizeof(ms_st)); ST(ms_st, s)->enable = 1; }
NVN(nvnMultisampleStateSetMultisampleEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->enable = B(e) != 0; }
NVN(nvnMultisampleStateSetSamples, void, NVNmultisampleState *s, int n) { ST(ms_st, s)->samples = (uint8_t)n; }
NVN(nvnMultisampleStateSetAlphaToCoverageEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->a2c = B(e) != 0; }
NVN(nvnMultisampleStateSetAlphaToCoverageDither, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->a2c_dither = B(e) != 0; }
NVN(nvnMultisampleStateSetRasterSamples, void, NVNmultisampleState *s, int n) { ST(ms_st, s)->raster_samples = (uint8_t)n; }
NVN(nvnMultisampleStateSetCoverageModulationMode, void, NVNmultisampleState *s, NVNcoverageModulationMode m) { ST(ms_st, s)->cov_mod = (uint8_t)m; }
NVN(nvnMultisampleStateSetCoverageToColorEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->cov2color_en = B(e) != 0; }
NVN(nvnMultisampleStateSetCoverageToColorOutput, void, NVNmultisampleState *s, int o) { ST(ms_st, s)->cov2color_out = (uint8_t)o; }
NVN(nvnMultisampleStateSetSampleLocationsEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->sl_en = B(e) != 0; }
NVN(nvnMultisampleStateSetSampleLocationsGridEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->sl_grid = B(e) != 0; }
static uint8_t sample_coord(float f) { int q = (int)(f * 16.0f); return (uint8_t)(q < 0 ? 0 : q > 15 ? 15 : q); }
/* v holds n (x, y) pairs in [0, 1) for locations start.. (in grid mode, location pixel * samples + sample). Metal has one set of positions for every
 * pixel, so only the first 8 locations (the first pixel of a grid) are kept. */
NVN(nvnMultisampleStateSetSampleLocations, void, NVNmultisampleState *s, int start, int n, const float *v) {
    ms_st *o = ST(ms_st, s);
    for (int i = 0; i < n && v; i++) {
        int idx = start + i;
        if (idx < 0 || idx >= 8) continue;
        o->sl_pos[idx] = (uint8_t)(sample_coord(v[2 * i]) | sample_coord(v[2 * i + 1]) << 4);
        if (idx + 1 > o->sl_n) o->sl_n = (uint8_t)(idx + 1);
    }
}
NVN(nvnMultisampleStateGetMultisampleEnable, NVNboolean, const NVNmultisampleState *s) { return CST(ms_st, s)->enable; }
NVN(nvnMultisampleStateGetSamples, int, const NVNmultisampleState *s) { return CST(ms_st, s)->samples; }
NVN(nvnMultisampleStateGetAlphaToCoverageEnable, NVNboolean, const NVNmultisampleState *s) { return CST(ms_st, s)->a2c; }
NVN(nvnMultisampleStateGetAlphaToCoverageDither, NVNboolean, const NVNmultisampleState *s) { return CST(ms_st, s)->a2c_dither; }
NVN(nvnMultisampleStateGetRasterSamples, int, NVNmultisampleState *s) { return CST(ms_st, s)->raster_samples; }
NVN(nvnMultisampleStateGetCoverageModulationMode, NVNcoverageModulationMode, const NVNmultisampleState *s) { return (NVNcoverageModulationMode)CST(ms_st, s)->cov_mod; }
NVN(nvnMultisampleStateGetCoverageToColorEnable, NVNboolean, const NVNmultisampleState *s) { return CST(ms_st, s)->cov2color_en; }
NVN(nvnMultisampleStateGetCoverageToColorOutput, int, const NVNmultisampleState *s) { return CST(ms_st, s)->cov2color_out; }
NVN(nvnMultisampleStateGetSampleLocationsEnable, NVNboolean, const NVNmultisampleState *s) { return CST(ms_st, s)->sl_en; }
NVN(nvnMultisampleStateGetSampleLocationsGridEnable, NVNboolean, const NVNmultisampleState *s) { return CST(ms_st, s)->sl_grid; }
/* The grid the programmable locations repeat over: the 16-entry location table divided by the sample count (4x4 pixels at 1 sample ... 1x1 at 16). */
NVN(nvnMultisampleStateGetSampleLocationsGrid, void, const NVNmultisampleState *s, int *w, int *h) {
    int n = CST(ms_st, s)->samples > 1 ? CST(ms_st, s)->samples : 1, cells = n >= 16 ? 1 : 16 / n, gw = 1, gh = 1;
    while (gw * gh < cells) { if (gw <= gh) gw *= 2; else gh *= 2; }
    if (w) *w = gw;
    if (h) *h = gh;
}

/* ---- polygon ---- */
NVN(nvnPolygonStateSetDefaults, void, NVNpolygonState *s) { *ST(poly_st, s) = (poly_st){ NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0 }; }
NVN(nvnPolygonStateSetCullFace, void, NVNpolygonState *s, NVNface f) { ST(poly_st, s)->cull = (uint32_t)f; }
NVN(nvnPolygonStateSetFrontFace, void, NVNpolygonState *s, NVNfrontFace f) { ST(poly_st, s)->front = (uint32_t)f; }
NVN(nvnPolygonStateSetPolygonMode, void, NVNpolygonState *s, NVNpolygonMode m) { ST(poly_st, s)->mode = (uint32_t)m; }
NVN(nvnPolygonStateSetPolygonOffsetEnables, void, NVNpolygonState *s, int e) { ST(poly_st, s)->offset_en = (uint32_t)e; }
NVN(nvnPolygonStateGetCullFace, NVNface, const NVNpolygonState *s) { return (NVNface)CST(poly_st, s)->cull; }
NVN(nvnPolygonStateGetFrontFace, NVNfrontFace, const NVNpolygonState *s) { return (NVNfrontFace)CST(poly_st, s)->front; }
NVN(nvnPolygonStateGetPolygonMode, NVNpolygonMode, const NVNpolygonState *s) { return (NVNpolygonMode)CST(poly_st, s)->mode; }
NVN(nvnPolygonStateGetPolygonOffsetEnables, NVNpolygonOffsetEnable, const NVNpolygonState *s) { return (NVNpolygonOffsetEnable)CST(poly_st, s)->offset_en; }

/* ---- depth / stencil ---- */
NVN(nvnDepthStencilStateSetDefaults, void, NVNdepthStencilState *s) {
    *ST(ds_st, s) = (ds_st){ 0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS,
                             NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP };
}
NVN(nvnDepthStencilStateSetDepthTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dt = B(e) != 0; }
NVN(nvnDepthStencilStateSetDepthWriteEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dw = B(e) != 0; }
NVN(nvnDepthStencilStateSetDepthFunc, void, NVNdepthStencilState *s, NVNdepthFunc f) { ST(ds_st, s)->df = (uint64_t)f; }
NVN(nvnDepthStencilStateSetStencilTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->st = B(e) != 0; }
NVN(nvnDepthStencilStateSetStencilFunc, void, NVNdepthStencilState *s, NVNface f, NVNstencilFunc fn) {
    ds_st *o = ST(ds_st, s);
    if (f & NVN_FACE_FRONT) o->sf_f = (uint64_t)fn;
    if (f & NVN_FACE_BACK) o->sf_b = (uint64_t)fn;
}
NVN(nvnDepthStencilStateSetStencilOp, void, NVNdepthStencilState *s, NVNface f, NVNstencilOp fail, NVNstencilOp zfail, NVNstencilOp zpass) {
    ds_st *o = ST(ds_st, s);
    if (f & NVN_FACE_FRONT) { o->fail_f = (uint64_t)fail; o->zfail_f = (uint64_t)zfail; o->zpass_f = (uint64_t)zpass; }
    if (f & NVN_FACE_BACK) { o->fail_b = (uint64_t)fail; o->zfail_b = (uint64_t)zfail; o->zpass_b = (uint64_t)zpass; }
}
NVN(nvnDepthStencilStateGetDepthTestEnable, NVNboolean, const NVNdepthStencilState *s) { return CST(ds_st, s)->dt; }
NVN(nvnDepthStencilStateGetDepthWriteEnable, NVNboolean, const NVNdepthStencilState *s) { return CST(ds_st, s)->dw; }
NVN(nvnDepthStencilStateGetDepthFunc, NVNdepthFunc, const NVNdepthStencilState *s) { return (NVNdepthFunc)CST(ds_st, s)->df; }
NVN(nvnDepthStencilStateGetStencilTestEnable, NVNboolean, const NVNdepthStencilState *s) { return CST(ds_st, s)->st; }
NVN(nvnDepthStencilStateGetStencilFunc, NVNstencilFunc, const NVNdepthStencilState *s, NVNface f) {
    return (NVNstencilFunc)((f & NVN_FACE_FRONT) ? CST(ds_st, s)->sf_f : CST(ds_st, s)->sf_b);
}
NVN(nvnDepthStencilStateGetStencilOp, void, const NVNdepthStencilState *s, NVNface f, NVNstencilOp *fail, NVNstencilOp *zfail, NVNstencilOp *zpass) {
    const ds_st *o = CST(ds_st, s);
    if (f & NVN_FACE_FRONT) { *fail = (NVNstencilOp)o->fail_f; *zfail = (NVNstencilOp)o->zfail_f; *zpass = (NVNstencilOp)o->zpass_f; }
    else { *fail = (NVNstencilOp)o->fail_b; *zfail = (NVNstencilOp)o->zfail_b; *zpass = (NVNstencilOp)o->zpass_b; }
}

/* ---- vertex attrib / stream ---- */
NVN(nvnVertexAttribStateSetDefaults, void, NVNvertexAttribState *s) { *ST(va_st, s) = (va_st){ 0, 0, 0 }; }
NVN(nvnVertexAttribStateSetFormat, void, NVNvertexAttribState *s, NVNformat f, ptrdiff_t off) {
    if (off < 0 || off >= (1 << 19)) LOG_ONCE(NAPPLE_LOG_WARN, "vertex attribute offset %td exceeds the packed range", off);
    ST(va_st, s)->fmt = (uint32_t)f; ST(va_st, s)->offset = (uint32_t)off;
}
NVN(nvnVertexAttribStateSetStreamIndex, void, NVNvertexAttribState *s, int i) { ST(va_st, s)->stream = (uint32_t)i; }
NVN(nvnVertexAttribStateGetFormat, void, const NVNvertexAttribState *s, NVNformat *f, ptrdiff_t *off) { *f = (NVNformat)CST(va_st, s)->fmt; *off = (ptrdiff_t)CST(va_st, s)->offset; }
NVN(nvnVertexAttribStateGetStreamIndex, int, const NVNvertexAttribState *s) { return (int)CST(va_st, s)->stream; }
NVN(nvnVertexStreamStateSetDefaults, void, NVNvertexStreamState *s) { *ST(vs_st, s) = (vs_st){ 0, 0 }; }
NVN(nvnVertexStreamStateSetStride, void, NVNvertexStreamState *s, ptrdiff_t st) { ST(vs_st, s)->stride = (uint32_t)st; }
NVN(nvnVertexStreamStateSetDivisor, void, NVNvertexStreamState *s, int d) { ST(vs_st, s)->divisor = (uint32_t)d; }
NVN(nvnVertexStreamStateGetStride, ptrdiff_t, const NVNvertexStreamState *s) { return (ptrdiff_t)CST(vs_st, s)->stride; }
NVN(nvnVertexStreamStateGetDivisor, int, const NVNvertexStreamState *s) { return (int)CST(vs_st, s)->divisor; }

/* ---- stage parameters the program's control section would carry (stages.h) ---- */
#include <pthread.h>
#include <stdlib.h>
#include "stages.h"
#define STAGE_PARAMS_MAX 256
static pthread_mutex_t g_sp_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { uint64_t hash; gfx_xfb_layout l; } g_xfb_tab[STAGE_PARAMS_MAX];
static struct { uint64_t hash; gfx_tess_params p; } g_tess_tab[STAGE_PARAMS_MAX];
static int g_nxfb, g_ntess;
void gfx_xfb_set_layout(uint64_t h, const gfx_xfb_layout *l) {
    if (!h) return;
    pthread_mutex_lock(&g_sp_mu);
    int i = 0;
    while (i < g_nxfb && g_xfb_tab[i].hash != h) i++;
    if (!l) { if (i < g_nxfb) g_xfb_tab[i] = g_xfb_tab[--g_nxfb]; }
    else if (i < g_nxfb || g_nxfb < STAGE_PARAMS_MAX) { if (i == g_nxfb) g_nxfb++; g_xfb_tab[i].hash = h; g_xfb_tab[i].l = *l; if (g_xfb_tab[i].l.count > MAX_XFB_ENTRIES) g_xfb_tab[i].l.count = MAX_XFB_ENTRIES; }
    else LOG_ONCE(NAPPLE_LOG_WARN, "transform feedback layout table full (%d programs)", STAGE_PARAMS_MAX);
    pthread_mutex_unlock(&g_sp_mu);
}
int gfx_xfb_get_layout(uint64_t h, gfx_xfb_layout *out) {
    int r = 0;
    pthread_mutex_lock(&g_sp_mu);
    for (int i = 0; i < g_nxfb; i++) if (g_xfb_tab[i].hash == h) { *out = g_xfb_tab[i].l; r = 1; break; }
    pthread_mutex_unlock(&g_sp_mu);
    return r;
}
void gfx_tess_set_params(uint64_t h, const gfx_tess_params *p) {
    if (!h) return;
    pthread_mutex_lock(&g_sp_mu);
    int i = 0;
    while (i < g_ntess && g_tess_tab[i].hash != h) i++;
    if (!p) { if (i < g_ntess) g_tess_tab[i] = g_tess_tab[--g_ntess]; }
    else if (i < g_ntess || g_ntess < STAGE_PARAMS_MAX) { if (i == g_ntess) g_ntess++; g_tess_tab[i].hash = h; g_tess_tab[i].p = *p; }
    else LOG_ONCE(NAPPLE_LOG_WARN, "tessellation parameter table full (%d programs)", STAGE_PARAMS_MAX);
    pthread_mutex_unlock(&g_sp_mu);
}
int gfx_tess_get_params(uint64_t h, gfx_tess_params *out) {
    static int def_domain = -1;
    if (def_domain < 0) { const char *e = getenv("NAPPLE_GFX_TESS_DOMAIN"); def_domain = e && !strcmp(e, "quad") ? TESS_DOMAIN_QUAD : TESS_DOMAIN_TRIANGLE; }
    *out = (gfx_tess_params){ (uint8_t)def_domain, TESS_SPACING_EQUAL, TESS_WINDING_CCW };
    int r = 0;
    pthread_mutex_lock(&g_sp_mu);
    for (int i = 0; i < g_ntess; i++) if (g_tess_tab[i].hash == h) { *out = g_tess_tab[i].p; r = 1; break; }
    pthread_mutex_unlock(&g_sp_mu);
    return r;
}
int gfx_program_set_xfb_layout(const void *prog, const gfx_xfb_layout *l) { /* keyed by the stage that feeds the rasterizer: geometry, else tess eval, else vertex */
    const prog_t *p = (const prog_t *)prog;
    uint64_t h = !p ? 0 : p->hash[2] ? p->hash[2] : p->hash[4] ? p->hash[4] : p->hash[0];
    if (!h) return 0;
    gfx_xfb_set_layout(h, l);
    return 1;
}
int gfx_program_set_tess_params(const void *prog, const gfx_tess_params *p) {
    uint64_t h = prog ? ((const prog_t *)prog)->hash[4] : 0;
    if (!h) return 0;
    gfx_tess_set_params(h, p);
    return 1;
}
