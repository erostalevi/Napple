#include "caps_core.h"
#include <string.h>

void caps_make_entry(CapsState *s, uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second, uint8_t out[CAPS_ENTRY_SIZE]) {
    memset(out, 0, CAPS_ENTRY_SIZE);
    /* +0 size (u64), +8 hash (u64), +0x10 AlbumFileDateTime {u16 year, month, day, hour, minute, second, unique id}, +0x18 storage, +0x19 content */
    memcpy(out + 0x10, &year, 2); out[0x12] = month; out[0x13] = day; out[0x14] = hour; out[0x15] = minute; out[0x16] = second;
    out[0x17] = (uint8_t)(s->saved & 0xff);
    out[0x19] = CAPS_CONTENT_SCREENSHOT;
    s->saved++;
}
