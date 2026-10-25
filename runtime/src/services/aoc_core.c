#include "aoc_core.h"
#include <stdio.h>
#include <string.h>

uint64_t aoc_base_id(uint64_t app) { return app + 0x1000; }
void aoc_init(AocSet *s, uint64_t app) { memset(s, 0, sizeof *s); s->base_id = aoc_base_id(app); }
int aoc_add(AocSet *s, uint64_t id, uint32_t version, const char *romfs) {
    if (s->n >= AOC_MAX || id <= s->base_id || id - s->base_id > 0xfff || !romfs) return 0;
    uint32_t index = (uint32_t)(id - s->base_id);
    for (unsigned i = 0; i < s->n; i++) if (s->e[i].index == index) return 0;
    unsigned at = s->n++;
    while (at > 0 && s->e[at - 1].index > index) { s->e[at] = s->e[at - 1]; at--; }
    AocEntry *e = &s->e[at]; e->title_id = id; e->index = index; e->version = version; snprintf(e->romfs, sizeof e->romfs, "%s", romfs);
    return 1;
}
unsigned aoc_count(const AocSet *s) { return s->n; }
unsigned aoc_list(const AocSet *s, uint32_t offset, uint32_t count, int32_t *out) {
    unsigned k = 0;
    for (uint32_t i = offset; i < s->n && k < count; i++) out[k++] = (int32_t)s->e[i].index;
    return k;
}
const AocEntry *aoc_find_index(const AocSet *s, int32_t index) { for (unsigned i = 0; i < s->n; i++) if ((int32_t)s->e[i].index == index) return &s->e[i]; return NULL; }
const AocEntry *aoc_find_data_id(const AocSet *s, uint64_t id) { for (unsigned i = 0; i < s->n; i++) if (s->e[i].title_id == id) return &s->e[i]; return NULL; }
