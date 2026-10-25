/* caps core: screenshot/album bookkeeping (portable). The album is an in-memory list of entries (no files, no persistence): a screenshot
 * save adds an entry with size 0, listing returns what was saved, deleting removes it. Loads of an existing entry yield 0 bytes. */
#pragma once
#include <stdint.h>

#define CAPS_ALBUM_MAX 64u
typedef struct { uint64_t saved; uint64_t shim_version; uint8_t album[CAPS_ALBUM_MAX][0x20]; unsigned count; } CapsState;
#define CAPS_ENTRY_SIZE 0x20u                       /* nn::capsrv ApplicationAlbumEntry */
enum { CAPS_CONTENT_SCREENSHOT = 0, CAPS_CONTENT_MOVIE = 1 };

/* Fills a 0x20-byte ApplicationAlbumEntry for a screenshot "saved" at the given civil time and counts it. size/hash are 0: there is no file. */
void caps_make_entry(CapsState *s, uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second, uint8_t out[CAPS_ENTRY_SIZE]);

/* The album, oldest first. Entries are identified by their AlbumFileDateTime + unique id (entry bytes 0x10..0x17) and content type (0x19);
 * the oldest entry is dropped when the album is full. caps_make_entry also records the entry. */
unsigned caps_album_count(const CapsState *s);
/* Copies up to `max` entries starting at `offset` that match `content` (-1 = any) into out (0x20 bytes each); returns how many were copied. */
unsigned caps_album_list(const CapsState *s, int content, unsigned offset, unsigned max, uint8_t *out);
/* Index of the entry whose datetime/unique id match entry[0x10..0x17], or -1. */
int caps_album_find(const CapsState *s, const uint8_t *entry);
int caps_album_delete(CapsState *s, const uint8_t *entry);   /* 0 on success, -1 when absent */
