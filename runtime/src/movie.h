/* Adapted from MacNativeGames (MIT), games/totk/runtime/movie_host.h. */
/* Host video decoder behind the SDK movie library's movie::Decoder (runtime/src/movie.c). */
#ifndef NAPPLE_MOVIE_H
#define NAPPLE_MOVIE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The guest functions the host decoder calls (nn::os::SignalEvent, movie::Buffer and movie::MediaData methods) and the
   original movie::Decoder code it hands other decoders to. movie.c fills them from the loaded modules;
    */
typedef struct {
    void (*signal_event)(void *event);
    void (*buf_set_range)(void *buf, int offset, int size);
    void (*buf_set_data)(void *buf, void *data, size_t capacity);
    void *(*buf_base)(const void *buf);
    size_t (*buf_capacity)(const void *buf);
    bool (*md_find_int32)(const void *md, const char *key, int *value);
    bool (*md_find_string)(const void *md, const char *key, const char **value);
    void (*md_set_int32)(void *md, const char *key, int value);
    void (*md_set_rect)(void *md, const char *key, int left, int top, int right, int bottom);
    void (*md_set_string)(void *md, const char *key, const char *value);
    void *vptr;                           /* vtable for movie::Decoder + 0x10: what the constructor stores at +0 */
    void *orig[20];                       /* original code per entry (MOVIE_* below), for decoders left to the library */
} MovieGuest;

enum { MOVIE_CTOR, MOVIE_D2, MOVIE_D0, MOVIE_CONFIGURE, MOVIE_REGISTER_EVENTS, MOVIE_GET_OUTPUT_FORMAT, MOVIE_START,
       MOVIE_STOP, MOVIE_RESET, MOVIE_FLUSH, MOVIE_ACQUIRE_INPUT, MOVIE_GET_INPUT, MOVIE_SEND_INPUT, MOVIE_ACQUIRE_OUTPUT,
       MOVIE_GET_OUTPUT, MOVIE_GET_OUTPUT_SIZE, MOVIE_RELEASE_OUTPUT, MOVIE_GET_LAST_ERROR, MOVIE_GET_OUTPUT_MD,
       MOVIE_GET_OUTPUT_BUFFER_FORMAT, MOVIE_ENTRIES };

void movie_register_overrides(void);                 /* host_symbols_register of every movie::Decoder entry (before modules are loaded) */
void movie_host_tick(void);                          /* every present: hand over decoded frames that are due */
#endif
