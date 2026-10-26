#include "caps_core.h"
#include "jpeg_core.h"
#include <stdlib.h>
#include <string.h>

void caps_make_entry(CapsState *s, uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second, uint8_t out[CAPS_ENTRY_SIZE]) {
    memset(out, 0, CAPS_ENTRY_SIZE);
    /* +0 size (u64), +8 hash (u64), +0x10 AlbumFileDateTime {u16 year, month, day, hour, minute, second, unique id}, +0x18 storage, +0x19 content */
    memcpy(out + 0x10, &year, 2);
    out[0x12] = month;
    out[0x13] = day;
    out[0x14] = hour;
    out[0x15] = minute;
    out[0x16] = second;
    out[0x17] = (uint8_t)(s->saved & 0xff);
    out[0x19] = CAPS_CONTENT_SCREENSHOT;
    s->saved++;
    if (s->count == CAPS_ALBUM_MAX) {
        free(s->jpeg[0]);
        memmove(s->album[0], s->album[1], (CAPS_ALBUM_MAX - 1) * CAPS_ENTRY_SIZE);
        memmove(&s->jpeg[0], &s->jpeg[1], (CAPS_ALBUM_MAX - 1) * sizeof s->jpeg[0]);
        memmove(&s->jpeg_size[0], &s->jpeg_size[1], (CAPS_ALBUM_MAX - 1) * sizeof s->jpeg_size[0]);
        s->count--;
    }
    s->jpeg[s->count] = NULL;
    s->jpeg_size[s->count] = 0;
    memcpy(s->album[s->count++], out, CAPS_ENTRY_SIZE);
}
unsigned caps_album_count(const CapsState *s) { return s->count; }
unsigned caps_album_list(const CapsState *s, int content, unsigned offset, unsigned max, uint8_t *out) {
    unsigned seen = 0, n = 0;
    for (unsigned i = 0; i < s->count && n < max; i++) {
        if (content >= 0 && s->album[i][0x19] != (uint8_t)content) continue;
        if (seen++ < offset) continue;
        memcpy(out + (size_t)n++ * CAPS_ENTRY_SIZE, s->album[i], CAPS_ENTRY_SIZE);
    }
    return n;
}
int caps_album_find(const CapsState *s, const uint8_t *entry) {
    for (unsigned i = 0; i < s->count; i++)
        if (!memcmp(s->album[i] + 0x10, entry + 0x10, 8)) return (int)i;
    return -1;
}
int caps_album_delete(CapsState *s, const uint8_t *entry) {
    int i = caps_album_find(s, entry);
    if (i < 0) return -1;
    free(s->jpeg[i]);
    unsigned tail = s->count - 1 - (unsigned)i;
    memmove(s->album[i], s->album[i + 1], (size_t)tail * CAPS_ENTRY_SIZE);
    memmove(&s->jpeg[i], &s->jpeg[i + 1], (size_t)tail * sizeof s->jpeg[0]);
    memmove(&s->jpeg_size[i], &s->jpeg_size[i + 1], (size_t)tail * sizeof s->jpeg_size[0]);
    s->count--;
    s->jpeg[s->count] = NULL;
    s->jpeg_size[s->count] = 0;
    return 0;
}

int caps_album_set_image(CapsState *s, const uint8_t *entry, const uint8_t *rgba, unsigned w, unsigned h, unsigned stride) {
    int i = caps_album_find(s, entry);
    if (i < 0) return -1;
    size_t n = 0;
    uint8_t *j = napjpeg_encode_rgba(rgba, (int)w, (int)h, (int)stride, 90, 1, &n);
    if (!j) return -2;
    free(s->jpeg[i]);
    s->jpeg[i] = j;
    s->jpeg_size[i] = n;
    uint64_t size = n, hash = 1469598103934665603ull; /* FNV-1a of the file */
    for (size_t k = 0; k < n; k++) hash = (hash ^ j[k]) * 1099511628211ull;
    memcpy(s->album[i], &size, 8);
    memcpy(s->album[i] + 8, &hash, 8);
    return 0;
}
int caps_album_entry(const CapsState *s, const uint8_t *entry, uint8_t out[CAPS_ENTRY_SIZE]) {
    int i = caps_album_find(s, entry);
    if (i < 0) return -1;
    memcpy(out, s->album[i], CAPS_ENTRY_SIZE);
    return 0;
}
int64_t caps_album_file_size(const CapsState *s, const uint8_t *entry) {
    int i = caps_album_find(s, entry);
    return i < 0 ? -1 : (int64_t)s->jpeg_size[i];
}
int64_t caps_album_load(const CapsState *s, const uint8_t *entry, int kind, uint8_t *dst, size_t cap) {
    int i = caps_album_find(s, entry);
    if (i < 0) return -1;
    if (!s->jpeg[i]) return 0;
    if (kind == 0) {
        if (cap < s->jpeg_size[i]) return -2;
        memcpy(dst, s->jpeg[i], s->jpeg_size[i]);
        return (int64_t)s->jpeg_size[i];
    }
    size_t need = kind == 1 ? CAPS_IMAGE_BYTES : CAPS_THUMB_BYTES;
    if (cap < need) return -2;
    int w = 0, h = 0, err = 0;
    uint8_t *px = napjpeg_decode_rgba(s->jpeg[i], s->jpeg_size[i], &w, &h, &err);
    if (!px || err == JPEGCORE_UNSUPPORTED) {
        free(px);
        return -3;
    }
    if (kind == 1 && (unsigned)w == CAPS_IMAGE_W && (unsigned)h == CAPS_IMAGE_H) memcpy(dst, px, need);
    else {
        uint8_t *t =
            kind == 1 ? napjpeg_scale_rgba(px, w, h, (int)CAPS_IMAGE_W, (int)CAPS_IMAGE_H) : napjpeg_scale_rgba(px, w, h, (int)CAPS_THUMB_W, (int)CAPS_THUMB_H);
        if (!t) {
            free(px);
            return -3;
        }
        memcpy(dst, t, need);
        free(t);
    }
    free(px);
    return (int64_t)need;
}
void caps_album_clear(CapsState *s) {
    for (unsigned i = 0; i < CAPS_ALBUM_MAX; i++) free(s->jpeg[i]);
    memset(s, 0, sizeof *s);
}
