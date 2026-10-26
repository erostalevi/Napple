/* set:fd core: the firmware-debug settings store (portable). In-memory name/key -> value map; nothing is persisted. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SETFD_MAX_ITEMS 64
#define SETFD_NAME_MAX 64
#define SETFD_VALUE_MAX 256
typedef struct {
    int used;
    char name[SETFD_NAME_MAX], key[SETFD_NAME_MAX];
    uint8_t value[SETFD_VALUE_MAX];
    size_t size;
} SetfdItem;
typedef struct {
    SetfdItem items[SETFD_MAX_ITEMS];
} SetfdStore;

/* 0 on success, -1 for an empty/oversized name or key, -2 for an oversized value, -3 when the store is full. Setting again replaces. */
int setfd_set(SetfdStore *s, const char *name, const char *key, const void *value, size_t size);
/* Returns the stored size (copying up to `cap` bytes) or -1 when absent. */
long setfd_get(const SetfdStore *s, const char *name, const char *key, void *out, size_t cap);
int setfd_reset(SetfdStore *s, const char *name, const char *key); /* 1 if something was removed */
/* The nth key (in insertion-slot order) of settings group `name`, or NULL. */
const char *setfd_nth_key(const SetfdStore *s, const char *name, unsigned n);
/* Copies a possibly unterminated buffer (guest strings) into dst, always NUL-terminating; returns 0, or -1 if it did not fit. */
int setfd_copy_string(char *dst, size_t cap, const void *src, size_t n);
