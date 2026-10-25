/* t_getters_linux.c - set -> get round trips of the NVN state/sampler/builder getters, portable (no Metal): builds and runs on Linux.
 * usage: sh tests/t_getters_linux.sh. The API sources are included directly so the static h_nvn* handlers can be called; symbols the
 * handlers reference but this test never calls are left unresolved by the script's linker flags. */
#include <stdio.h>
#include <stdlib.h>
#include "../src/api_state.c"
#include "../src/api_texture.c"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void t_blend(void) {
    NVNblendState s; h_nvnBlendStateSetDefaults(&s);
    h_nvnBlendStateSetBlendTarget(&s, 5);
    h_nvnBlendStateSetBlendFunc(&s, NVN_BLEND_FUNC_SRC_ALPHA, NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO);
    h_nvnBlendStateSetBlendEquation(&s, NVN_BLEND_EQUATION_SUB, NVN_BLEND_EQUATION_MAX);
    int a, b, c, d;
    CHECK(h_nvnBlendStateGetBlendTarget(&s) == 5);
    h_nvnBlendStateGetBlendFunc(&s, &a, &b, &c, &d);
    CHECK(a == NVN_BLEND_FUNC_SRC_ALPHA && b == NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA && c == NVN_BLEND_FUNC_ONE && d == NVN_BLEND_FUNC_ZERO);
    h_nvnBlendStateGetBlendEquation(&s, &a, &b);
    CHECK(a == NVN_BLEND_EQUATION_SUB && b == NVN_BLEND_EQUATION_MAX);
    CHECK(h_nvnBlendStateGetAdvancedMode(&s) == 0 && h_nvnBlendStateGetAdvancedPremultipliedSrc(&s) && h_nvnBlendStateGetAdvancedNormalizedDst(&s));
    h_nvnBlendStateSetAdvancedMode(&s, 9); h_nvnBlendStateSetAdvancedOverlap(&s, 2);
    h_nvnBlendStateSetAdvancedPremultipliedSrc(&s, 0); h_nvnBlendStateSetAdvancedNormalizedDst(&s, 0);
    CHECK(h_nvnBlendStateGetAdvancedMode(&s) == 9 && h_nvnBlendStateGetAdvancedOverlap(&s) == 2);
    CHECK(!h_nvnBlendStateGetAdvancedPremultipliedSrc(&s) && !h_nvnBlendStateGetAdvancedNormalizedDst(&s));
    CHECK(h_nvnBlendStateGetBlendTarget(&s) == 5);   /* advanced fields do not clobber the rest */
}
static void t_color(void) {
    NVNcolorState s; h_nvnColorStateSetDefaults(&s);
    CHECK(h_nvnColorStateGetLogicOp(&s) == NVN_LOGIC_OP_COPY && h_nvnColorStateGetAlphaTest(&s) == NVN_ALPHA_FUNC_ALWAYS);
    h_nvnColorStateSetBlendEnable(&s, 3, 1); h_nvnColorStateSetLogicOp(&s, NVN_LOGIC_OP_XOR); h_nvnColorStateSetAlphaTest(&s, NVN_ALPHA_FUNC_GREATER);
    CHECK(h_nvnColorStateGetBlendEnable(&s, 3) && !h_nvnColorStateGetBlendEnable(&s, 2) && !h_nvnColorStateGetBlendEnable(&s, 9));
    CHECK(h_nvnColorStateGetLogicOp(&s) == NVN_LOGIC_OP_XOR && h_nvnColorStateGetAlphaTest(&s) == NVN_ALPHA_FUNC_GREATER);
}
static void t_chmask(void) {
    NVNchannelMaskState s; h_nvnChannelMaskStateSetDefaults(&s);
    h_nvnChannelMaskStateSetChannelMask(&s, 2, 1, 0, 1, 0);
    NVNboolean r, g, b, a;
    h_nvnChannelMaskStateGetChannelMask(&s, 2, &r, &g, &b, &a);
    CHECK(r == 1 && g == 0 && b == 1 && a == 0);
    h_nvnChannelMaskStateGetChannelMask(&s, 0, &r, &g, &b, &a);
    CHECK(r && g && b && a);
}
static void t_ms(void) {
    NVNmultisampleState s; h_nvnMultisampleStateSetDefaults(&s);
    int w, h;
    h_nvnMultisampleStateSetSamples(&s, 4);
    h_nvnMultisampleStateGetSampleLocationsGrid(&s, &w, &h);
    CHECK(w * h == 4);
    h_nvnMultisampleStateSetSamples(&s, 1);
    h_nvnMultisampleStateGetSampleLocationsGrid(&s, &w, &h);
    CHECK(w * h == 16);
}
static void t_sampler(void) {
    samp_builder_t bb; memset(&bb, 0, sizeof bb);
    NVNsamplerBuilder *b = (NVNsamplerBuilder *)&bb;
    NVNdevice *dev = (NVNdevice *)0x1000;
    h_nvnSamplerBuilderSetDevice(b, dev); h_nvnSamplerBuilderSetDefaults(b);
    CHECK(h_nvnSamplerBuilderGetDevice(b) == dev);
    h_nvnSamplerBuilderSetMinMagFilter(b, NVN_MIN_FILTER_LINEAR, NVN_MAG_FILTER_NEAREST);
    h_nvnSamplerBuilderSetWrapMode(b, NVN_WRAP_MODE_CLAMP, NVN_WRAP_MODE_MIRRORED_REPEAT, NVN_WRAP_MODE_MIRROR_CLAMP);
    h_nvnSamplerBuilderSetLodClamp(b, 1.5f, 7.0f); h_nvnSamplerBuilderSetLodBias(b, -0.5f);
    h_nvnSamplerBuilderSetCompare(b, NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE, NVN_COMPARE_FUNC_GEQUAL);
    float bc[4] = { 0.25f, 0.5f, 0.75f, 1.0f }; h_nvnSamplerBuilderSetBorderColor(b, bc);
    h_nvnSamplerBuilderSetMaxAnisotropy(b, 8.0f); h_nvnSamplerBuilderSetReductionFilter(b, NVN_SAMPLER_REDUCTION_MAX);
    NVNminFilter mn; NVNmagFilter mg; NVNwrapMode ws, wt, wr; float l0, l1, f4[4]; NVNcompareMode cm; NVNcompareFunc cf;
    h_nvnSamplerBuilderGetMinMagFilter(b, &mn, &mg); CHECK(mn == NVN_MIN_FILTER_LINEAR && mg == NVN_MAG_FILTER_NEAREST);
    h_nvnSamplerBuilderGetWrapMode(b, &ws, &wt, &wr); CHECK(ws == NVN_WRAP_MODE_CLAMP && wt == NVN_WRAP_MODE_MIRRORED_REPEAT && wr == NVN_WRAP_MODE_MIRROR_CLAMP);
    h_nvnSamplerBuilderGetLodClamp(b, &l0, &l1); CHECK(l0 == 1.5f && l1 == 7.0f);
    CHECK(h_nvnSamplerBuilderGetLodBias(b) == -0.5f);
    h_nvnSamplerBuilderGetCompare(b, &cm, &cf); CHECK(cm == NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE && cf == NVN_COMPARE_FUNC_GEQUAL);
    h_nvnSamplerBuilderGetBorderColor(b, f4); CHECK(f4[0] == 0.25f && f4[3] == 1.0f);
    CHECK(h_nvnSamplerBuilderGetMaxAnisotropy(b) == 8.0f && h_nvnSamplerBuilderGetReductionFilter(b) == NVN_SAMPLER_REDUCTION_MAX);
    int ci[4] = { -1, 2, 3, 4 }, ro[4]; h_nvnSamplerBuilderSetBorderColori(b, ci); h_nvnSamplerBuilderGetBorderColori(b, ro); CHECK(ro[0] == -1 && ro[3] == 4);
    uint32_t cu[4] = { 5, 6, 7, 8 }, ru[4]; h_nvnSamplerBuilderSetBorderColorui(b, cu); h_nvnSamplerBuilderGetBorderColorui(b, ru); CHECK(ru[1] == 6 && ru[2] == 7);

    /* the built sampler reports the same */
    samp_t so; NVNsampler *s = (NVNsampler *)&so;
    CHECK(h_nvnSamplerInitialize(s, b));
    h_nvnSamplerGetMinMagFilter(s, &mn, &mg); CHECK(mn == NVN_MIN_FILTER_LINEAR && mg == NVN_MAG_FILTER_NEAREST);
    h_nvnSamplerGetWrapMode(s, &ws, &wt, &wr); CHECK(ws == NVN_WRAP_MODE_CLAMP && wr == NVN_WRAP_MODE_MIRROR_CLAMP);
    h_nvnSamplerGetLodClamp(s, &l0, &l1); CHECK(l0 == 1.5f && l1 == 7.0f && h_nvnSamplerGetLodBias(s) == -0.5f);
    h_nvnSamplerGetCompare(s, &cm, &cf); CHECK(cf == NVN_COMPARE_FUNC_GEQUAL);
    h_nvnSamplerGetBorderColori(s, ro); CHECK(ro[0] == 5);   /* i/ui/float setters share one border colour */
    h_nvnSamplerGetBorderColorui(s, ru); CHECK(ru[0] == 5);
    CHECK(h_nvnSamplerGetMaxAnisotropy(s) == 8.0f && h_nvnSamplerGetReductionFilter(s) == NVN_SAMPLER_REDUCTION_MAX);
    CHECK(h_nvnSamplerCompare(s, s));
}

int main(void) {
    t_blend(); t_color(); t_chmask(); t_ms(); t_sampler();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
