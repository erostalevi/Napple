/* control.c - reader of the GLSLC control section (layout notes in control.h). Portable C; tests/t_control.c. */
#include <string.h>

#include "control.h"

static uint32_t rd32(const uint8_t *p, size_t off) {
    uint32_t v;
    memcpy(&v, p + off, 4);
    return v;
}

uint32_t gfx_control_length(const uint8_t *ctl) {
    uint32_t n = rd32(ctl, 0x20);
    return n >= GLSLC_CONTROL_MIN && n <= GLSLC_CONTROL_MAX && !(n & 3) ? n : 0x880u;
}

int gfx_control_parse(const uint8_t *ctl, size_t len, gfx_control_info *info) {
    memset(info, 0, sizeof *info);
    if (!ctl || (len && len < GLSLC_CONTROL_MIN)) return 0;
    info->length = gfx_control_length(ctl);
    info->const_size = rd32(ctl, 0x6fc);
    info->const_off = rd32(ctl, 0x700);
    info->code_len = rd32(ctl, 0x704);
    info->stage = rd32(ctl, 0x714);
    for (int i = 0; i < 3; i++) info->local[i] = rd32(ctl, 0x748 + 4 * (size_t)i);
    info->shared_mem = rd32(ctl, 0x754);
    info->valid = rd32(ctl, 0) == GLSLC_CONTROL_MAGIC && info->stage <= 5 && info->code_len && info->code_len <= (16u << 20) && info->const_off <= info->code_len &&
                  info->const_size <= 0x10000 && (!len || len >= info->length);
    return info->valid;
}

int gfx_xfb_layout_from_varyings(const uint16_t stride[MAX_XFB_BUFFERS], const uint8_t count[MAX_XFB_BUFFERS], const uint8_t locs[MAX_XFB_BUFFERS][128],
                                 gfx_xfb_layout *out) {
    memset(out, 0, sizeof *out);
    for (int k = 0; k < MAX_XFB_BUFFERS; k++) {
        if (!count[k]) continue;
        if (!stride[k] || (stride[k] & 3) || count[k] > 128) return 0;
        out->stride[k] = stride[k];
        int run = 0; /* consecutive components of one attribute become one entry */
        for (int i = 0; i < count[k]; i++) {
            uint8_t c = locs[k][i];
            if ((uint32_t)i * 4 + 4 > stride[k]) return 0;
            if (c == 0xff) { run = 0; continue; }
            uint16_t addr = (uint16_t)c * 4;
            if (run && out->count && out->e[out->count - 1].addr + out->e[out->count - 1].comps * 4 == addr && (addr & 0xf) &&
                out->e[out->count - 1].offset + out->e[out->count - 1].comps * 4 == i * 4) {
                out->e[out->count - 1].comps++;
                continue;
            }
            if (out->count >= MAX_XFB_ENTRIES) return 0;
            out->e[out->count++] = (ShaderXfbEntry){addr, (uint8_t)k, 1, (uint16_t)(i * 4)};
            run = 1;
        }
    }
    return out->count > 0;
}

int gfx_control_xfb(const uint8_t *ctl, size_t len, gfx_xfb_layout *out) {
    gfx_control_info ci;
    if (!gfx_control_parse(ctl, len, &ci) || (ci.stage != 0 && ci.stage != 2 && ci.stage != 4)) return 0;
    if (ci.length < GLSLC_XFB_LOCS_OFF + 4 * 128) return 0;
    uint16_t stride[MAX_XFB_BUFFERS];
    uint8_t count[MAX_XFB_BUFFERS], locs[MAX_XFB_BUFFERS][128];
    int any = 0;
    for (int k = 0; k < MAX_XFB_BUFFERS; k++) {
        uint32_t s = rd32(ctl, GLSLC_XFB_STRIDE_OFF + 4 * (size_t)k), n = rd32(ctl, GLSLC_XFB_COUNT_OFF + 4 * (size_t)k);
        if (s > 2048 || (s & 3) || n > 128 || (n && !s)) return 0;
        stride[k] = (uint16_t)s;
        count[k] = (uint8_t)n;
        memcpy(locs[k], ctl + GLSLC_XFB_LOCS_OFF + 128 * (size_t)k, 128);
        for (uint32_t i = 0; i < n; i++)
            if (locs[k][i] != 0xff && locs[k][i] >= 0xc0) return 0;
        for (uint32_t i = n; i < 128; i++)
            if (locs[k][i] && locs[k][i] != 0xff) return 0; /* unused tail must be clear */
        any |= n > 0;
    }
    if (!any) return 0;
    return gfx_xfb_layout_from_varyings(stride, count, locs, out);
}

int gfx_control_tess(const uint8_t *ctl, size_t len, gfx_tess_params *out) {
    gfx_control_info ci;
    if (!gfx_control_parse(ctl, len, &ci) || ci.stage != 4 || ci.length <= GLSLC_TESS_MODE_OFF) return 0;
    uint32_t m = rd32(ctl, GLSLC_TESS_MODE_OFF);
    if (!m || (m & ~0x333u)) return 0; /* bits beyond the Maxwell form: not that word */
    uint32_t dom = m & 3, sp = (m >> 4) & 3;
    if (dom == 3 || sp == 3) return 0;
    out->domain = dom == 0 ? TESS_DOMAIN_ISOLINE : dom == 1 ? TESS_DOMAIN_TRIANGLE : TESS_DOMAIN_QUAD;
    out->spacing = sp == 1 ? TESS_SPACING_FRACTIONAL_ODD : sp == 2 ? TESS_SPACING_FRACTIONAL_EVEN : TESS_SPACING_EQUAL;
    out->winding = (m >> 8) & 1 ? TESS_WINDING_CW : TESS_WINDING_CCW;
    return 1;
}
