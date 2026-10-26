/* Unit tests: caps album images. A save that carries a 1280x720 RGBA buffer is stored as JPEG (jpeg_core); size, file load, decoded image and
 * decoded thumbnail come back through the caps:u and session handlers. Synthetic gradients only. */
#include "../src/services/caps.c"
#include "svc_test.h"
#include <math.h>
#ifdef __linux__
int applet_screenshot_permitted(void) { return 1; } /* applet.c is not linked on Linux */
#endif

static void gradient(uint8_t *p) {
    for (unsigned y = 0; y < CAPS_IMAGE_H; y++)
        for (unsigned x = 0; x < CAPS_IMAGE_W; x++) {
            uint8_t *o = p + ((size_t)y * CAPS_IMAGE_W + x) * 4;
            o[0] = (uint8_t)(x * 255 / (CAPS_IMAGE_W - 1)); o[1] = (uint8_t)(y * 255 / (CAPS_IMAGE_H - 1)); o[2] = (uint8_t)(128 + (x / 160) * 16); o[3] = 255;
        }
}
static double mean_abs(const uint8_t *a, const uint8_t *b, size_t px) {
    double s = 0; for (size_t i = 0; i < px; i++) for (int k = 0; k < 3; k++) s += fabs((double)a[i * 4 + k] - b[i * 4 + k]);
    return s / (double)(px * 3);
}
int main(void) {
    uint8_t *img = malloc(CAPS_IMAGE_BYTES), *out = malloc(CAPS_IMAGE_BYTES);
    gradient(img);
    IpcObject *su = su_open(), *u = u_open(); IpcCall c;

    /* save with pixels */
    memset(&c, 0, sizeof c); svc_set_in_buf(&c, img, CAPS_IMAGE_BYTES); svc_call_c(su, 32, &c);
    CHECK(c.result == 0 && c.out_size == CAPS_ENTRY_SIZE, "save with pixels succeeds");
    uint8_t e[CAPS_ENTRY_SIZE]; memcpy(e, c.out, sizeof e);
    uint64_t fsize, fhash; memcpy(&fsize, e, 8); memcpy(&fhash, e + 8, 8);
    CHECK(fsize > 1000 && fsize < CAPS_IMAGE_BYTES / 4 && fhash != 0, "entry carries the JPEG size (%llu) and hash", (unsigned long long)fsize);
    CHECK(caps_album_count(&caps) == 1 && caps.jpeg[0] && caps.jpeg_size[0] == fsize, "album holds the JPEG");

    svc_call(u, 104, e, sizeof e, &c); CHECK(c.result == 0 && (uint64_t)SVC_OUT(&c, int64_t, 0) == fsize, "GetAlbumFileSize = JPEG size");

    /* decoded image */
    memset(&c, 0, sizeof c); c.in = e; c.in_size = sizeof e; svc_set_out_buf(&c, out, CAPS_IMAGE_BYTES); svc_call_c(u, 110, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, int64_t, 0) == (int64_t)CAPS_IMAGE_BYTES, "LoadAlbumScreenShotImage writes 1280x720 RGBA");
    double d = mean_abs(img, out, (size_t)CAPS_IMAGE_W * CAPS_IMAGE_H);
    CHECK(d < 3.0, "decoded image is close to the saved one (mean abs error %.2f)", d);
    CHECK(out[3] == 255 && out[(CAPS_IMAGE_BYTES) - 1] == 255, "alpha is opaque");

    /* thumbnail */
    memset(out, 0, 64); memset(&c, 0, sizeof c); c.in = e; c.in_size = sizeof e; svc_set_out_buf(&c, out, CAPS_THUMB_BYTES); svc_call_c(u, 120, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, int64_t, 0) == (int64_t)CAPS_THUMB_BYTES, "thumbnail is 320x180 RGBA");
    CHECK(out[0] < 8 && out[(CAPS_THUMB_W - 1) * 4] > 247, "thumbnail keeps the left-to-right ramp (R %u .. %u)", out[0], out[(CAPS_THUMB_W - 1) * 4]);

    /* the JPEG file through a session */
    svc_call(u, 60002, NULL, 0, &c); IpcObject *sess = c.out_objects[0];
    uint8_t *file = malloc(fsize + 16);
    memset(&c, 0, sizeof c); c.in = e; c.in_size = sizeof e; svc_set_out_buf(&c, file, fsize + 16); svc_call_c(sess, 2, &c);
    CHECK(c.result == 0 && (uint64_t)SVC_OUT(&c, int64_t, 0) == fsize && file[0] == 0xff && file[1] == 0xd8 && file[fsize - 2] == 0xff && file[fsize - 1] == 0xd9, "session LoadAlbumFile returns the JPEG file");
    memset(&c, 0, sizeof c); c.in = e; c.in_size = sizeof e; svc_set_out_buf(&c, out, CAPS_THUMB_BYTES); svc_call_c(sess, 5, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, int64_t, 0) == (int64_t)CAPS_THUMB_BYTES, "session thumbnail");
    memset(&c, 0, sizeof c); c.in = e; c.in_size = sizeof e; svc_set_out_buf(&c, out, 100); svc_call_c(u, 110, &c);
    CHECK(c.result == HZ_R_CAPTURE_INVALID_ARGUMENT, "output buffer too small is refused");
    memset(&c, 0, sizeof c); c.in = e; c.in_size = sizeof e; svc_call_c(u, 110, &c);
    CHECK(c.result == HZ_R_CAPTURE_INVALID_ARGUMENT, "no output buffer is refused");

    /* a save without pixels next to it keeps the old behaviour */
    svc_call(su, 32, NULL, 0, &c); uint8_t bare[CAPS_ENTRY_SIZE]; memcpy(bare, c.out, sizeof bare);
    uint64_t z; memcpy(&z, bare, 8); CHECK(z == 0 && caps.jpeg[1] == NULL, "save without pixels: size 0, no image");
    memset(&c, 0, sizeof c); c.in = bare; c.in_size = sizeof bare; svc_set_out_buf(&c, out, CAPS_IMAGE_BYTES); svc_call_c(u, 110, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, int64_t, 0) == 0, "load of an entry without image: 0 bytes");

    /* a short buffer is not an image */
    memset(&c, 0, sizeof c); svc_set_in_buf(&c, img, 1000); svc_call_c(su, 32, &c); uint8_t shortb[CAPS_ENTRY_SIZE]; memcpy(shortb, c.out, sizeof shortb);
    CHECK(caps.jpeg[2] == NULL, "a buffer smaller than a capture is ignored");

    /* delete frees the image, the next entry keeps its own */
    svc_call(u, 103, e, sizeof e, &c); CHECK(c.result == 0 && caps_album_count(&caps) == 2 && caps.jpeg[0] == NULL, "delete drops the image and shifts the album");
    svc_call(u, 104, e, sizeof e, &c); CHECK(c.result == HZ_R_CAPTURE_NOT_FOUND, "deleted entry is gone");

    /* fill past the cap with images: the oldest are dropped without leaks (ASAN) and indexes stay in step */
    for (unsigned i = 0; i < CAPS_ALBUM_MAX + 2; i++) { img[0] = (uint8_t)i; memset(&c, 0, sizeof c); svc_set_in_buf(&c, img, CAPS_IMAGE_BYTES); svc_call_c(su, 32, &c); }
    unsigned with = 0; for (unsigned i = 0; i < caps.count; i++) with += caps.jpeg[i] != NULL;
    CHECK(caps.count == CAPS_ALBUM_MAX && with >= CAPS_ALBUM_MAX - 2, "album capped with images attached (%u of %u)", with, caps.count);
    for (unsigned i = 0; i < caps.count; i++) if (caps.jpeg[i]) { uint64_t sz; memcpy(&sz, caps.album[i], 8); CHECK(sz == caps.jpeg_size[i], "entry %u size matches its image", i); break; }

    caps_album_clear(&caps);
    free(file); free(img); free(out);
    printf("{\"test\":\"svc_caps_image\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
