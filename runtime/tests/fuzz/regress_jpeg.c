/* Regression tests for the crashes the jpeg fuzz target found (FINDINGS.md). Build with the sanitizers: make -C runtime fuzz-regress. */
#include "../../src/services/jpeg_core.h"
#include "../check.h"

int main(void) {
    /* A frame header with 4 components (CMYK): napjpeg_info refused it after storing ncomp = 4 and then copied f.h[0..3] / f.v[0..3] into the 3-entry
     * arrays of JpegInfo (stack out-of-bounds write; UBSan: index 3 out of bounds for type 'int[3]'). Found by fuzz jpeg, seed 1, iteration 204. */
    static const uint8_t cmyk[] = {0xff, 0xd8, 0xff, 0xc0, 0x00, 0x14, 0x08, 0x00, 0x10, 0x00, 0x10, 0x04, 1,
                                   0x11, 0,    2,    0x11, 0,    3,    0x11, 0,    4,    0x11, 0,    0xff, 0xd9};
    struct {
        JpegInfo info;
        uint32_t canary[4];
    } s;
    memset(&s, 0xA5, sizeof s);
    int rc = napjpeg_info(cmyk, sizeof cmyk, &s.info);
    CHECK(rc == JPEGCORE_UNSUPPORTED, "4-component frame header is unsupported (got %d)", rc);
    CHECK(s.canary[0] == 0xA5A5A5A5 && s.canary[1] == 0xA5A5A5A5, "napjpeg_info wrote past JpegInfo");
    int w, h, err = 0;
    CHECK(napjpeg_decode_rgba(cmyk, sizeof cmyk, &w, &h, &err) == NULL && err == JPEGCORE_UNSUPPORTED, "decode refuses the 4-component stream (err %d)", err);

    /* 2. A Huffman table whose code lengths over-subscribe the code space (three 1-bit codes): huff_build wrote look[512 + j] past the 512 entry lookahead
     * table (ASan: heap-buffer-overflow in huff_build; this is AC table 3, the last of the eight, so the write leaves the allocation). Found by fuzz jpeg, seed 11, iteration 27664 (453 byte input). The table is refused as a bad stream. */
    static const uint8_t dht[] = {0xff, 0xd8, 0xff, 0xc4, 0x00, 0x16, 0x13, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 0xff, 0xd9};
    err = 0;
    CHECK(napjpeg_decode_rgba(dht, sizeof dht, &w, &h, &err) == NULL && err == JPEGCORE_BAD, "an over-subscribed Huffman table is a bad stream (err %d)", err);
    printf("{\"test\":\"regress_jpeg\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
