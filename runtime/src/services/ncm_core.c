#include "ncm_core.h"
#include <string.h>

int ncm_meta_parse(const uint8_t *b, size_t n, NcmMetaView *v) {
    memset(v, 0, sizeof *v);
    if (!b || n < 8) return -1;
    v->ext_size = (uint16_t)(b[0] | b[1] << 8);
    v->content_count = (uint16_t)(b[2] | b[3] << 8);
    v->meta_count = (uint16_t)(b[4] | b[5] << 8);
    v->attributes = b[6];
    v->storage_id = b[7];
    size_t need = 8 + (size_t)v->ext_size + (size_t)v->content_count * NCM_CONTENT_INFO_SIZE + (size_t)v->meta_count * NCM_META_INFO_SIZE;
    if (need > n) {
        memset(v, 0, sizeof *v);
        return -1;
    }
    v->contents = b + 8 + v->ext_size;
    v->metas = v->contents + (size_t)v->content_count * NCM_CONTENT_INFO_SIZE;
    return 0;
}
int ncm_meta_find_type(const NcmMetaView *v, uint8_t type, uint8_t id_out[16]) {
    for (unsigned i = 0; i < v->content_count; i++) {
        const uint8_t *e = v->contents + (size_t)i * NCM_CONTENT_INFO_SIZE;
        if (e[23] == type) {
            memcpy(id_out, e, 16);
            return 0;
        }
    }
    return -1;
}
int ncm_meta_has_content(const NcmMetaView *v, const uint8_t id[16]) {
    for (unsigned i = 0; i < v->content_count; i++)
        if (!memcmp(v->contents + (size_t)i * NCM_CONTENT_INFO_SIZE, id, 16)) return 1;
    return 0;
}
