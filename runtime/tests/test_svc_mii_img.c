/* Unit tests: the Mii image database core and the miiimg service rows. */
#include "../src/services/mii_img.c"
#include "svc_test.h"
#include "../src/services/jpeg_core.h"

static void uid(uint8_t u[16], int n) { memset(u, 0, 16); u[0] = (uint8_t)n; u[15] = (uint8_t)(n * 3); }

int main(void) {
    MiiImgDb d; memset(&d, 0, sizeof d); miiimg_init(&d);
    uint8_t u1[16], u2[16]; uid(u1, 1); uid(u2, 2);
    size_t n1 = 0, n1b = 0, n2 = 0; uint8_t *g1 = miiimg_generate(u1, &n1), *g1b = miiimg_generate(u1, &n1b), *g2 = miiimg_generate(u2, &n2);
    CHECK(g1 && n1 > 200 && n1 < MIIIMG_MAX_IMAGE, "a face image is generated (%zu bytes)", n1);
    CHECK(g1b && n1b == n1 && !memcmp(g1, g1b, n1), "generation is deterministic");
    CHECK(g2 && (n2 != n1 || memcmp(g1, g2, n1) != 0), "a different id gives a different face");
    JpegInfo info; CHECK(napjpeg_info(g1, n1, &info) == JPEGCORE_OK && info.width == MIIIMG_GEN_SIZE && info.height == MIIIMG_GEN_SIZE, "the generated image is a %dx%d JPEG", MIIIMG_GEN_SIZE, MIIIMG_GEN_SIZE);
    int w = 0, h = 0, err = 0; uint8_t *rgba = napjpeg_decode_rgba(g1, n1, &w, &h, &err); CHECK(rgba && w == MIIIMG_GEN_SIZE && err == 0, "it decodes");
    CHECK(rgba && memcmp(rgba, rgba + ((size_t)(h / 2) * w + w / 2) * 4, 3) != 0, "the picture is not flat");
    free(rgba); free(g1); free(g1b); free(g2);

    CHECK(miiimg_count(&d) == 0 && miiimg_free_slots(&d) == MIIIMG_CAPACITY, "empty database");
    int i1 = miiimg_add(&d, u1, NULL, 0), i2 = miiimg_add(&d, u2, "\xff\xd8junk", 6);
    CHECK(i1 == 0 && i2 == 1 && miiimg_count(&d) == 2 && d.e[0].flags == 1 && d.e[1].flags == 0 && d.e[1].size == 6 && d.e[1].width == 0, "generated and supplied entries (a non-image blob is stored as is)");
    CHECK(miiimg_add(&d, u1, "\xff\xd8xyz", 5) == 0 && miiimg_count(&d) == 2 && d.e[0].size == 5 && d.e[0].flags == 0, "an existing id is replaced in place");
    uint8_t a[MIIIMG_ATTR_SIZE]; miiimg_attribute(&d.e[1], a); CHECK(!memcmp(a, u2, 16) && *(uint32_t *)(a + 24) == 6, "attribute record");
    CHECK(miiimg_find(&d, u2) == 1 && miiimg_find(&d, (uint8_t[16]){9}) == -1, "find by id");
    CHECK(miiimg_delete_index(&d, 0) == 0 && miiimg_count(&d) == 1 && miiimg_find(&d, u2) == 0 && miiimg_delete_index(&d, 5) == -1, "delete moves later entries down");
    for (int i = 10; i < 10 + (int)MIIIMG_CAPACITY; i++) { uint8_t u[16]; uid(u, i); miiimg_add(&d, u, "\xff\xd8z", 3); }
    { uint8_t u[16]; uid(u, 200); CHECK(miiimg_count(&d) == MIIIMG_CAPACITY && miiimg_free_slots(&d) == 0 && miiimg_add(&d, u, "\xff\xd8z", 3) == -1, "a full database refuses a new id"); }
    { static uint8_t big[MIIIMG_MAX_IMAGE + 1]; CHECK(miiimg_add(&d, u2, big, sizeof big) == -1, "an oversized image is refused"); }
    miiimg_init(&d);

    /* service rows */
    IpcObject *s = img_open(); IpcCall c;
    svc_call(s, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "GetCount starts at 0");
    svc_call(s, 3, NULL, 0, &c); CHECK(c.out[0] == 1, "IsEmpty"); svc_call(s, 4, NULL, 0, &c); CHECK(c.out[0] == 0, "not full");
    svc_call(s, 11, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == (int)MIIIMG_CAPACITY, "GetFreeSlotCount");
    uint32_t idx = 0; svc_call(s, 5, &idx, 4, &c); CHECK(c.result == MIIIMG_R_NOT_FOUND, "GetAttribute on an empty database: not found");
    svc_call(s, 6, &idx, 4, &c); CHECK(c.result == MIIIMG_R_NOT_FOUND, "LoadImage on an empty database: not found");
    svc_call(s, 7, u1, 16, &c); CHECK(c.result == 0, "AddOrUpdateImage without an image generates one");
    svc_call(s, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 1, "count 1"); svc_call(s, 3, NULL, 0, &c); CHECK(c.out[0] == 0, "not empty");
    svc_call(s, 5, &idx, 4, &c); CHECK(c.result == 0 && c.out_size == MIIIMG_ATTR_SIZE && !memcmp(c.out, u1, 16) && SVC_OUT(&c, uint32_t, 16) == MIIIMG_GEN_SIZE, "GetAttribute: id, size 96x96");
    { static uint8_t img[0x4000]; memset(&c, 0, sizeof c); c.in = (const uint8_t *)&idx; c.in_size = 4; svc_set_out_buf(&c, img, sizeof img); svc_call_c(s, 6, &c);
      uint32_t sz = SVC_OUT(&c, uint32_t, 0); CHECK(c.result == 0 && sz > 200 && sz < sizeof img, "LoadImage returns the size (%u)", sz);
      JpegInfo ji; CHECK(napjpeg_info(img, sz, &ji) == JPEGCORE_OK, "LoadImage returns a decodable JPEG");
      uint8_t part[16]; memset(&c, 0, sizeof c); c.in = (const uint8_t *)&idx; c.in_size = 4; svc_set_out_buf(&c, part, sizeof part); svc_call_c(s, 6, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 16 && !memcmp(part, img, 16), "a short buffer gets a prefix"); }
    { uint8_t two[16]; uid(two, 2); static const uint8_t jpg[] = {0xff, 0xd8, 1, 2, 3}; memset(&c, 0, sizeof c); c.in = two; c.in_size = 16; svc_set_in_buf(&c, jpg, sizeof jpg); svc_call_c(s, 7, &c);
      CHECK(c.result == 0, "AddOrUpdateImage with a supplied image"); idx = 1; svc_call(s, 5, &idx, 4, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 24) == 5, "the supplied image is stored as given"); }
    svc_call(s, 7, NULL, 0, &c); CHECK(c.result != 0, "AddOrUpdateImage without an id fails");
    idx = 0; svc_call(s, 8, &idx, 4, &c); CHECK(c.result == 0, "DeleteImages"); svc_call(s, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 1, "one left");
    idx = 9; svc_call(s, 8, &idx, 4, &c); CHECK(c.result == MIIIMG_R_NOT_FOUND, "DeleteImages out of range");
    svc_call(s, 9, NULL, 0, &c); svc_call(s, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "DeleteFile empties the database");
    svc_call(s, 7, u1, 16, &c); svc_call(s, 13, NULL, 0, &c); svc_call(s, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "ForceInitialize empties it too");
    svc_call(s, 7, u1, 16, &c); svc_call(s, 0, NULL, 0, &c); svc_call(s, 1, NULL, 0, &c); svc_call(s, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 1, "Initialize and Reload keep the entries");
    svc_call(s, 12, NULL, 0, &c); CHECK(c.result == 0, "ExportFile: success"); svc_call(s, 14, NULL, 0, &c); svc_call(s, 15, NULL, 0, &c); CHECK(c.result == 0, "reserved rows answer");
    printf("{\"test\":\"svc_mii_img\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
