#include "setfd_core.h"
#include <string.h>

int setfd_copy_string(char *dst, size_t cap, const void *src, size_t n) {
    size_t len = 0;
    const char *p = src;
    while (len < n && p[len]) len++;
    if (len >= cap) {
        if (cap) dst[0] = 0;
        return -1;
    }
    memcpy(dst, p, len);
    dst[len] = 0;
    return 0;
}
static SetfdItem *find(const SetfdStore *s, const char *name, const char *key) {
    for (int i = 0; i < SETFD_MAX_ITEMS; i++)
        if (s->items[i].used && !strcmp(s->items[i].name, name) && !strcmp(s->items[i].key, key)) return (SetfdItem *)&s->items[i];
    return NULL;
}
int setfd_set(SetfdStore *s, const char *name, const char *key, const void *value, size_t size) {
    if (!name[0] || !key[0] || strlen(name) >= SETFD_NAME_MAX || strlen(key) >= SETFD_NAME_MAX) return -1;
    if (size > SETFD_VALUE_MAX) return -2;
    SetfdItem *it = find(s, name, key);
    if (!it) {
        for (int i = 0; i < SETFD_MAX_ITEMS; i++)
            if (!s->items[i].used) {
                it = &s->items[i];
                break;
            }
    }
    if (!it) return -3;
    it->used = 1;
    strcpy(it->name, name);
    strcpy(it->key, key);
    memcpy(it->value, value, size);
    it->size = size;
    return 0;
}
long setfd_get(const SetfdStore *s, const char *name, const char *key, void *out, size_t cap) {
    const SetfdItem *it = find(s, name, key);
    if (!it) return -1;
    memcpy(out, it->value, it->size < cap ? it->size : cap);
    return (long)it->size;
}
int setfd_reset(SetfdStore *s, const char *name, const char *key) {
    SetfdItem *it = find(s, name, key);
    if (!it) return 0;
    it->used = 0;
    return 1;
}
const char *setfd_nth_key(const SetfdStore *s, const char *name, unsigned n) {
    for (int i = 0; i < SETFD_MAX_ITEMS; i++)
        if (s->items[i].used && !strcmp(s->items[i].name, name) && n-- == 0) return s->items[i].key;
    return NULL;
}
