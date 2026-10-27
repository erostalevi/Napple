#include "mii_img_core.h"
#include "jpeg_core.h"
#include <stdlib.h>
#include <string.h>

void miiimg_init(MiiImgDb *d) {
    for (unsigned i = 0; i < d->count; i++) free(d->e[i].data);
    memset(d, 0, sizeof *d);
    d->initialized = 1;
}
unsigned miiimg_count(const MiiImgDb *d) { return d->count; }
unsigned miiimg_free_slots(const MiiImgDb *d) { return MIIIMG_CAPACITY - d->count; }
int miiimg_find(const MiiImgDb *d, const uint8_t uid[16]) {
    for (unsigned i = 0; i < d->count; i++)
        if (!memcmp(d->e[i].uid, uid, 16)) return (int)i;
    return -1;
}

static uint32_t hash32(const uint8_t *p, size_t n, uint32_t h) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
static void px(uint8_t *img, int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || y < 0 || x >= MIIIMG_GEN_SIZE || y >= MIIIMG_GEN_SIZE) return;
    uint8_t *p = img + ((size_t)y * MIIIMG_GEN_SIZE + (size_t)x) * 4;
    p[0] = r, p[1] = g, p[2] = b, p[3] = 255;
}
static void disc(uint8_t *img, int cx, int cy, int rx, int ry, uint8_t r, uint8_t g, uint8_t b) {
    for (int y = cy - ry; y <= cy + ry; y++)
        for (int x = cx - rx; x <= cx + rx; x++) {
            long dx = x - cx, dy = y - cy;
            if (dx * dx * (long)ry * ry + dy * dy * (long)rx * rx <= (long)rx * rx * ry * ry) px(img, x, y, r, g, b);
        }
}
uint8_t *miiimg_generate(const uint8_t uid[16], size_t *out_size) {
    uint32_t h = hash32(uid, 16, 2166136261u);
    static const uint8_t skin[6][3] = {{255, 224, 189}, {241, 194, 125}, {224, 172, 105}, {198, 134, 66}, {141, 85, 36}, {255, 205, 148}};
    const uint8_t *sk = skin[h % 6];
    uint8_t hair[3] = {(uint8_t)(40 + ((h >> 8) & 0x7f)), (uint8_t)(30 + ((h >> 15) & 0x5f)), (uint8_t)(20 + ((h >> 22) & 0x4f))};
    uint8_t *img = malloc((size_t)MIIIMG_GEN_SIZE * MIIIMG_GEN_SIZE * 4);
    if (!img) return NULL;
    for (int y = 0; y < MIIIMG_GEN_SIZE; y++)
        for (int x = 0; x < MIIIMG_GEN_SIZE; x++) px(img, x, y, 235, 240, 245);
    int cx = MIIIMG_GEN_SIZE / 2, cy = MIIIMG_GEN_SIZE / 2 + 4;
    disc(img, cx, cy - 6, 34, 30, hair[0], hair[1], hair[2]);  /* hair behind the face */
    disc(img, cx, cy, 30, 34, sk[0], sk[1], sk[2]);            /* face */
    disc(img, cx, cy - 26, 30, 10, hair[0], hair[1], hair[2]); /* fringe */
    int eye = 8 + (int)((h >> 4) & 3);
    disc(img, cx - eye, cy - 2, 3, 4, 30, 30, 40);
    disc(img, cx + eye, cy - 2, 3, 4, 30, 30, 40);
    int mw = 8 + (int)((h >> 28) & 3) * 2;
    for (int x = cx - mw; x <= cx + mw; x++) {
        int dy = (x - cx) * (x - cx) / (mw * 2 > 0 ? mw * 2 : 1);
        px(img, x, cy + 16 + dy, 150, 60, 60);
        px(img, x, cy + 17 + dy, 150, 60, 60);
    }
    uint8_t *jpg = napjpeg_encode_rgba(img, MIIIMG_GEN_SIZE, MIIIMG_GEN_SIZE, MIIIMG_GEN_SIZE * 4, 85, 0, out_size);
    free(img);
    return jpg;
}

int miiimg_add(MiiImgDb *d, const uint8_t uid[16], const void *image, size_t n) {
    if (n > MIIIMG_MAX_IMAGE) return -1;
    int idx = miiimg_find(d, uid);
    if (idx < 0 && d->count >= MIIIMG_CAPACITY) return -1;
    uint8_t *data;
    size_t size = n;
    uint32_t flags = 0;
    if (n) {
        data = malloc(n);
        if (!data) return -1;
        memcpy(data, image, n);
    } else {
        data = miiimg_generate(uid, &size);
        flags = 1;
        if (!data) return -1;
    }
    JpegInfo info;
    int ok = napjpeg_info(data, size, &info) == JPEGCORE_OK;
    if (idx < 0) idx = (int)d->count++;
    else free(d->e[idx].data);
    MiiImgEntry *e = &d->e[idx];
    memcpy(e->uid, uid, 16);
    e->width = ok ? (uint32_t)info.width : 0;
    e->height = ok ? (uint32_t)info.height : 0;
    e->size = (uint32_t)size;
    e->flags = flags;
    e->data = data;
    return idx;
}
int miiimg_delete_index(MiiImgDb *d, unsigned index) {
    if (index >= d->count) return -1;
    free(d->e[index].data);
    memmove(&d->e[index], &d->e[index + 1], (d->count - index - 1) * sizeof d->e[0]);
    d->count--;
    memset(&d->e[d->count], 0, sizeof d->e[0]);
    return 0;
}
void miiimg_attribute(const MiiImgEntry *e, uint8_t out[MIIIMG_ATTR_SIZE]) {
    memset(out, 0, MIIIMG_ATTR_SIZE);
    memcpy(out, e->uid, 16);
    memcpy(out + 16, &e->width, 4);
    memcpy(out + 20, &e->height, 4);
    memcpy(out + 24, &e->size, 4);
    memcpy(out + 28, &e->flags, 4);
}
