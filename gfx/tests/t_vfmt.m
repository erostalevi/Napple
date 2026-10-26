/* t_vfmt.m - NVN vertex attribute formats -> Metal vertex formats. Regression for the Link's Awakening in-game white-out: RGB10A2SN (0x69), the packed
 * normal/tangent format of its meshes, had no mapping, the attribute read as zero, lighting normalised a zero vector and every lit surface saturated. */
#import <Metal/Metal.h>
#include "nvn_test.h"
#include "mtl_priv.h"

static void expect(int nvn, MTLVertexFormat want, int want_kind, const char *name) {
    int kind = -1;
    uint32_t got = mtl_vertex_format(nvn, &kind);
    CHECK(got == (uint32_t)want && kind == want_kind, "%s (%#x): got format %u kind %d, want %lu kind %d", name, nvn, got, kind, (unsigned long)want,
          want_kind);
}
int main(void) {
    expect(0x22, MTLVertexFormatFloat3, 0, "RGB32F");
    expect(0x25, MTLVertexFormatUChar4Normalized, 0, "RGBA8");
    expect(0x26, MTLVertexFormatChar4Normalized, 0, "RGBA8SN");
    expect(0x27, MTLVertexFormatUChar4, 2, "RGBA8UI");
    expect(0x13, MTLVertexFormatShort2Normalized, 0, "RG16SN");
    expect(0x3D, MTLVertexFormatUInt1010102Normalized, 0, "RGB10A2");
    expect(0x69, MTLVertexFormatInt1010102Normalized, 0, "RGB10A2SN");
    expect(0x51, MTLVertexFormatUChar, 0, "R8_UI2F");
    expect(0x52, MTLVertexFormatChar, 0, "R8_I2F");
    expect(0x5B, MTLVertexFormatUInt2, 0, "RG32_UI2F");
    expect(0x66, MTLVertexFormatShort4, 0, "RGBA16_I2F");
    expect(0x9A, MTLVertexFormatUChar4Normalized_BGRA, 0, "BGRA8");
    CHECK(mtl_vertex_format(0x00, &(int){0}) == (uint32_t)MTLVertexFormatInvalid, "NONE has no format");
    printf(g_fail ? "t_vfmt: %d FAILED\n" : "t_vfmt: ok\n", g_fail);
    return g_fail != 0;
}
