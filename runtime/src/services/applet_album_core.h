/* Photo viewer / album library applet (0x15), portable, over the caps album (caps_core.h, public functions only). The guest pushes CommonArguments,
 * then an argument storage { u8 mode; u8 content (0 screenshots, 1 movies, 0xff any); u8 pad[2]; } (4 bytes; shorter or missing = view, any). Answer
 * AlbumResult { u32 result; u32 selected; u8 entry[0x20] } (0x28): result 0 = success, 2 = cancelled; `selected` is 1 when an entry was picked.
 * Layouts are [unverified] against a title. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "applet_lib_core.h"
#include "caps_core.h"

enum { LIBAPPLET_ID_PHOTO_VIEWER = 0x15 };
enum { ALBUM_MODE_VIEW = 0, ALBUM_MODE_SELECT = 1 }; /* select: the user must pick one image for the game */
enum { ALBUM_RESULT_OK = 0, ALBUM_RESULT_CANCELLED = 2 };
#define ALBUM_RESULT_SIZE 0x28u
typedef struct {
    uint8_t mode;
    int content; /* CAPS_CONTENT_* or -1 for any */
} AlbumArgs;
void album_args_parse(const uint8_t *data, size_t size, AlbumArgs *a);
/* Entries visible under the args, newest first is not applied: oldest first like caps. Returns how many were copied into out (0x20 each). */
unsigned album_list(const CapsState *s, const AlbumArgs *a, uint8_t (*out)[CAPS_ENTRY_SIZE], unsigned max);
void album_result_encode(uint32_t result, const uint8_t *entry /* NULL = none */, uint8_t out[ALBUM_RESULT_SIZE]);
/* Walks the list: each image is loaded (decoded, CAPS_IMAGE_W x H) and shown through applet_ui_show_image; the first one the user picks is the
 * result. View mode never selects (always returns to the game); select mode with no pick or an empty album is cancelled. Returns the result code and
 * fills `picked` (0x20 bytes) when an entry was selected (*has_pick = 1). */
uint32_t album_run(const CapsState *s, const AlbumArgs *a, uint8_t picked[CAPS_ENTRY_SIZE], int *has_pick);

/* The caps state the album is read from (caps.c keeps its state static: the service registers it here). NULL = an empty album. */
void applet_album_set_source(const CapsState *s);
int applet_album_register(void); /* 0x15 through libapplet_register_handler */
int applet_album_handler(LibAppletCall *call);
