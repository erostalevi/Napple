#include "caps_core.h"
#include <string.h>

void caps_make_entry(CapsState *s, uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second, uint8_t out[CAPS_ENTRY_SIZE]) {
    memset(out, 0, CAPS_ENTRY_SIZE);
    /* +0 size (u64), +8 hash (u64), +0x10 AlbumFileDateTime {u16 year, month, day, hour, minute, second, unique id}, +0x18 storage, +0x19 content */
    memcpy(out + 0x10, &year, 2); out[0x12] = month; out[0x13] = day; out[0x14] = hour; out[0x15] = minute; out[0x16] = second;
    out[0x17] = (uint8_t)(s->saved & 0xff);
    out[0x19] = CAPS_CONTENT_SCREENSHOT;
    s->saved++;
    if (s->count == CAPS_ALBUM_MAX) { memmove(s->album[0], s->album[1], (CAPS_ALBUM_MAX - 1) * CAPS_ENTRY_SIZE); s->count--; }
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
    for (unsigned i = 0; i < s->count; i++) if (!memcmp(s->album[i] + 0x10, entry + 0x10, 8)) return (int)i;
    return -1;
}
int caps_album_delete(CapsState *s, const uint8_t *entry) {
    int i = caps_album_find(s, entry); if (i < 0) return -1;
    memmove(s->album[i], s->album[i + 1], (size_t)(s->count - 1 - (unsigned)i) * CAPS_ENTRY_SIZE); s->count--;
    return 0;
}
