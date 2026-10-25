/* Unit tests: pl:u serves a valid placeholder font when system/fonts is missing (no zero-size font reaches the guest). */
#include "../src/services/pl.c"
#include "svc_test.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

int main(void) {
    uint8_t ttf[512]; uint32_t n = pl_placeholder(ttf);
    CHECK(n > 0 && n <= sizeof ttf, "placeholder fits its buffer");
    CHECK(be32(ttf) == 0x00010000 && ttf[5] == 10, "sfnt header: TrueType, 10 tables");
    for (int i = 0; i < 10; i++) { const uint8_t *r = ttf + 12 + 16 * i; uint32_t off = be32(r + 8), len = be32(r + 12); CHECK(off % 4 == 0 && off + len <= n, "table inside blob"); }

    g_cfg.save_root = "/nonexistent/napple-pl-test";
    IpcObject *o = pl_open(); IpcCall c;
    for (uint32_t t = 0; t < NFONT; t++) {
        svc_call(o, 2, &t, 4, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == n, "GetSize is the placeholder size, never 0");
        svc_call(o, 3, &t, 4, &c); CHECK(SVC_OUT(&c, uint32_t, 0) >= 8, "GetSharedMemoryAddressOffset points past the header");
    }
    uint32_t o0, o1; svc_call(o, 3, &(uint32_t){0}, 4, &c); o0 = SVC_OUT(&c, uint32_t, 0); svc_call(o, 3, &(uint32_t){1}, 4, &c); o1 = SVC_OUT(&c, uint32_t, 0);
    CHECK(o0 != o1 && o1 >= o0 + n, "each type has its own non-overlapping copy (sharing one offset crashed LAS)");
    uint32_t bad = 99; svc_call(o, 2, &bad, 4, &c); CHECK(c.result != 0, "bad font type rejected");
    CHECK(pl.loaded == 0 && pl.size[0] == n, "no real fonts loaded");
    const uint8_t *blob = (uint8_t *)pl.shm->host + pl.offset[0];
    CHECK(be32(blob) == 0x00010000, "shared memory holds the font at the offset");
    uint32_t hdr[2]; memcpy(hdr, blob - 8, 8); CHECK(hdr[0] == 0x18029A7Fu && (hdr[1] ^ 0x49621806u) == n, "shared-font header");
    printf("{\"test\":\"svc_pl\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
