/* Unit tests: caps:dc DecodeJpeg (caps_dc.c over jpeg_core): a JPEG in, the requested size of RGBA out; scaling, short buffers and garbage refused. */
#include "../src/services/caps_dc.c"
#include "svc_test.h"

int main(void) {
    IpcObject *dc = dc_open(); IpcCall c;
    enum { W = 48, H = 32 };
    static uint8_t px[W * H * 4], out[W * H * 4];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { uint8_t *p = px + (y * W + x) * 4; p[0] = (uint8_t)(x * 5); p[1] = (uint8_t)(y * 7); p[2] = 120; p[3] = 255; }
    size_t n; uint8_t *jpg = napjpeg_encode_rgba(px, W, H, W * 4, 92, 1, &n);
    CHECK(jpg && n > 100, "encoded a %dx%d JPEG (%zu bytes)", W, H, n);
    uint32_t args[10] = {W, H}; /* width, height, ScreenShotDecodeOption (0x20 bytes, ignored) */
    c = (IpcCall){0}; svc_set_in_buf(&c, jpg, n); svc_set_out_buf(&c, out, sizeof out); c.in = (const uint8_t *)args; c.in_size = sizeof args;
    svc_call_c(dc, 3001, &c);
    long err = 0; for (int i = 0; i < W * H; i++) for (int k = 0; k < 3; k++) err += abs((int)out[i * 4 + k] - (int)px[i * 4 + k]);
    CHECK(c.result == 0 && out[3] == 255 && err / (W * H * 3) < 6, "DecodeJpeg at the file size: result %#x, mean error %ld", c.result, err / (W * H * 3));
    memset(out, 0, sizeof out); uint32_t half[10] = {W / 2, H / 2};
    c = (IpcCall){0}; svc_set_in_buf(&c, jpg, n); svc_set_out_buf(&c, out, sizeof out); c.in = (const uint8_t *)half; c.in_size = sizeof half;
    svc_call_c(dc, 3001, &c);
    CHECK(c.result == 0 && out[(W / 2 - 1) * 4] > out[0] + 60 && out[(W / 2) * (H / 2) * 4] == 0, "DecodeJpeg scaled to %dx%d, nothing written past it", W / 2, H / 2);
    c = (IpcCall){0}; svc_set_in_buf(&c, jpg, n); svc_set_out_buf(&c, out, W * H * 4 - 4); c.in = (const uint8_t *)args; c.in_size = sizeof args;
    svc_call_c(dc, 3001, &c); CHECK(c.result == HZ_R_CAPTURE_INVALID_ARGUMENT, "output buffer too small: invalid argument");
    uint8_t junk[64] = {0xff, 0xd8, 0xff, 0xe0, 1, 2, 3};
    c = (IpcCall){0}; svc_set_in_buf(&c, junk, sizeof junk); svc_set_out_buf(&c, out, sizeof out); c.in = (const uint8_t *)args; c.in_size = sizeof args;
    svc_call_c(dc, 3001, &c); CHECK(c.result == HZ_R_CAPTURE_INVALID_ARGUMENT, "garbage JPEG: invalid argument");
    uint32_t zero[10] = {0, H};
    c = (IpcCall){0}; svc_set_in_buf(&c, jpg, n); svc_set_out_buf(&c, out, sizeof out); c.in = (const uint8_t *)zero; c.in_size = sizeof zero;
    svc_call_c(dc, 3001, &c); CHECK(c.result == HZ_R_CAPTURE_INVALID_ARGUMENT, "zero width: invalid argument");
    c = (IpcCall){0}; svc_set_out_buf(&c, out, sizeof out); c.in = (const uint8_t *)args; c.in_size = sizeof args;
    svc_call_c(dc, 3001, &c); CHECK(c.result == HZ_R_CAPTURE_INVALID_ARGUMENT, "no input buffer: invalid argument");
    free(jpg);
    printf("{\"test\":\"svc_caps_dc\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
