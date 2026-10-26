/* caps core: screenshot/album bookkeeping (portable). The album is an in-memory list of entries (no files, no persistence): a screenshot
 * save adds an entry, listing returns what was saved, deleting removes it. A save that carries pixels (media-services 2026-10-26) is stored as
 * a JPEG (jpeg_core, the same codec nvjpg uses): the entry then has a file size, and loads give back the JPEG file, the decoded image or the
 * decoded thumbnail. A save without pixels keeps the earlier behaviour: size 0 and loads that yield 0 bytes. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define CAPS_ALBUM_MAX 64u
typedef struct {
    uint64_t saved;
    uint64_t shim_version;
    uint8_t album[CAPS_ALBUM_MAX][0x20];
    unsigned count;
    uint8_t *jpeg[CAPS_ALBUM_MAX]; /* parallel to album: the saved JPEG file, or NULL */
    size_t jpeg_size[CAPS_ALBUM_MAX];
} CapsState;
#define CAPS_IMAGE_W 1280u /* capture size of the screenshots (RGBA8) */
#define CAPS_IMAGE_H 720u
#define CAPS_THUMB_W 320u
#define CAPS_THUMB_H 180u
#define CAPS_IMAGE_BYTES (CAPS_IMAGE_W * CAPS_IMAGE_H * 4u)
#define CAPS_THUMB_BYTES (CAPS_THUMB_W * CAPS_THUMB_H * 4u)
#define CAPS_ENTRY_SIZE 0x20u /* nn::capsrv ApplicationAlbumEntry */
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
int caps_album_delete(CapsState *s, const uint8_t *entry); /* 0 on success, -1 when absent */

/* Stores pixels (RGBA8, w x h, row stride in bytes) for the album entry whose datetime/unique id match entry[0x10..0x17]: encodes a JPEG, records its
 * size (entry +0) and a hash (entry +8) in the album copy. Returns 0, -1 for an unknown entry, -2 when encoding failed. */
int caps_album_set_image(CapsState *s, const uint8_t *entry, const uint8_t *rgba, unsigned w, unsigned h, unsigned stride);
/* Copies the stored 0x20-byte entry (with size and hash filled in) for `entry`; returns 0 or -1. */
int caps_album_entry(const CapsState *s, const uint8_t *entry, uint8_t out[CAPS_ENTRY_SIZE]);
/* File size in bytes of the stored JPEG (0 when the entry has no image), or -1 for an unknown entry. */
int64_t caps_album_file_size(const CapsState *s, const uint8_t *entry);
/* What a load writes: kind 0 = the JPEG file, 1 = the decoded image (CAPS_IMAGE_W x CAPS_IMAGE_H RGBA), 2 = the decoded thumbnail (CAPS_THUMB_W x
 * CAPS_THUMB_H RGBA; the image box-filtered down). Fills dst (up to cap bytes) and returns the byte count the kind needs: 0 when the entry has no
 * image, -1 for an unknown entry, -2 when cap is too small (nothing written), -3 when the stored JPEG cannot be decoded. */
int64_t caps_album_load(const CapsState *s, const uint8_t *entry, int kind, uint8_t *dst, size_t cap);
/* Frees every stored image (the state itself is static in the service, this is for tests). */
void caps_album_clear(CapsState *s);
