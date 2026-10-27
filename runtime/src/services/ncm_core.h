/* ncm core: reads the content infos out of a stored ContentMeta blob (portable). The installed layout is the public one (switchbrew "NCM services"): an
 * 8-byte header {u16 extended header size, u16 content count, u16 content meta count, u8 attributes, u8 storage id}, the extended header, then the
 * ContentInfo records (0x18 bytes: ContentId[16], u8 size[6], u8 attributes, u8 type) and the ContentMetaInfo records (0x10 bytes: u64 id, u32 version,
 * u8 type, u8 attributes, 2 bytes padding). Layouts are from the author's memory of that description (unverified). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define NCM_CONTENT_INFO_SIZE 0x18u
#define NCM_META_INFO_SIZE 0x10u
typedef struct {
    uint16_t ext_size, content_count, meta_count;
    uint8_t attributes, storage_id;
    const uint8_t *contents, *metas; /* point into the blob; valid only when the counts fit */
} NcmMetaView;

/* 0 on success; -1 when the blob is shorter than its own header claims. */
int ncm_meta_parse(const uint8_t *blob, size_t n, NcmMetaView *v);
/* The ContentId (16 bytes) of the first content of `type` (0 meta, 1 program, 2 data, 3 control, 4 html doc, 5 legal info, 6 delta), or -1. */
int ncm_meta_find_type(const NcmMetaView *v, uint8_t type, uint8_t id_out[16]);
/* 1 when `id` is one of the contents. */
int ncm_meta_has_content(const NcmMetaView *v, const uint8_t id[16]);
