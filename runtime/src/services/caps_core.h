/* caps core: screenshot/album bookkeeping (portable). The host keeps no album: screenshot saves succeed without writing a file and the
 * album is always empty. */
#pragma once
#include <stdint.h>

typedef struct { uint64_t saved; uint64_t shim_version; } CapsState;
#define CAPS_ENTRY_SIZE 0x20u                       /* nn::capsrv ApplicationAlbumEntry */
enum { CAPS_CONTENT_SCREENSHOT = 0, CAPS_CONTENT_MOVIE = 1 };

/* Fills a 0x20-byte ApplicationAlbumEntry for a screenshot "saved" at the given civil time and counts it. size/hash are 0: there is no file. */
void caps_make_entry(CapsState *s, uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second, uint8_t out[CAPS_ENTRY_SIZE]);
