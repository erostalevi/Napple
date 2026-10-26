/* ncm: content manager. The host runs one unpacked title from a data directory, so both content-meta databases and the content storages
 * start empty. They are real in-memory tables though: what the guest registers (Set, CreatePlaceHolder, Register) is what later queries
 * see, until Remove/Delete or process exit. Nothing is persisted and no content file exists, so GetPath still answers ContentNotFound.
 * Command ids and layouts are from the author's memory of the public documentation and are unverified; unlisted commands fail loudly. */
#include "svc_common.h"
#include "hz_result.h"
#include <string.h>

#define NCM_MAX_METAS 64
#define NCM_MAX_META_DATA 0x400u
#define NCM_MAX_CONTENTS 128
#define NCM_KEY_SIZE 0x10u /* ContentMetaKey {u64 id, u32 version, u8 type, u8 install type, u8 pad[2]} */
#define NCM_ID_SIZE 0x10u  /* ContentId / PlaceHolderId (16 bytes) */
#define NCM_PATH_SIZE 0x300u
enum { NCM_TYPE_APPLICATION = 0x80, NCM_TYPE_PATCH = 0x81, NCM_TYPE_ADDON = 0x82 };

typedef struct {
    uint8_t used, key[NCM_KEY_SIZE];
    uint32_t size;
    uint8_t data[NCM_MAX_META_DATA];
} NcmMeta;
typedef struct {
    uint8_t used, placeholder, id[NCM_ID_SIZE];
    uint64_t size;
} NcmContent;
static NcmMeta metas[NCM_MAX_METAS];
static NcmContent contents[NCM_MAX_CONTENTS];

static uint64_t key_id(const uint8_t *k) {
    uint64_t v;
    memcpy(&v, k, 8);
    return v;
}
static uint32_t key_version(const uint8_t *k) {
    uint32_t v;
    memcpy(&v, k + 8, 4);
    return v;
}
static NcmMeta *meta_find(const uint8_t *key) {
    for (int i = 0; i < NCM_MAX_METAS; i++)
        if (metas[i].used && !memcmp(metas[i].key, key, 12) && metas[i].key[12] == key[12]) return &metas[i];
    return NULL;
}
static NcmContent *content_find(const uint8_t *id, int placeholder) {
    for (int i = 0; i < NCM_MAX_CONTENTS; i++)
        if (contents[i].used && contents[i].placeholder == placeholder && !memcmp(contents[i].id, id, NCM_ID_SIZE)) return &contents[i];
    return NULL;
}
static NcmContent *content_add(const uint8_t *id, int placeholder, uint64_t size) {
    NcmContent *e = content_find(id, placeholder);
    for (int i = 0; !e && i < NCM_MAX_CONTENTS; i++)
        if (!contents[i].used) e = &contents[i];
    if (!e) return NULL;
    e->used = 1;
    e->placeholder = (uint8_t)placeholder;
    memcpy(e->id, id, NCM_ID_SIZE);
    e->size = size;
    return e;
}
static const uint8_t *in_key(IpcCall *c, uint32_t off) { return (const uint8_t *)ipc_in(c, off, NCM_KEY_SIZE); }
static const uint8_t *in_id(IpcCall *c, uint32_t off) { return (const uint8_t *)ipc_in(c, off, NCM_ID_SIZE); }

/* ---- IContentMetaDatabase ---- */
static void meta_set(IpcObject *s, IpcCall *c) {
    (void)s;
    const uint8_t *key = in_key(c, 0);
    const IpcBuf *b = ipc_in_buf(c, 0);
    NcmMeta *m = meta_find(key);
    for (int i = 0; !m && i < NCM_MAX_METAS; i++)
        if (!metas[i].used) m = &metas[i];
    if (!m) {
        c->result = HZ_R_NCM_CONTENT_META_NOT_FOUND;
        return;
    } /* table full (no better code known) */
    uint64_t n = b ? b->size : 0;
    if (n > NCM_MAX_META_DATA) n = NCM_MAX_META_DATA;
    m->used = 1;
    memcpy(m->key, key, NCM_KEY_SIZE);
    m->size = (uint32_t)n;
    if (n) memcpy(m->data, ipc_buf_ptr(b), n);
}
static void meta_get(IpcObject *s, IpcCall *c) {
    (void)s;
    NcmMeta *m = meta_find(in_key(c, 0));
    if (!m) {
        c->result = HZ_R_NCM_CONTENT_META_NOT_FOUND;
        return;
    }
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint64_t n = b ? ipc_write_buf(c, b, m->data, m->size) : 0;
    ipc_out(c, &n, 8);
}
static void meta_remove(IpcObject *s, IpcCall *c) {
    (void)s;
    NcmMeta *m = meta_find(in_key(c, 0));
    if (!m) {
        c->result = HZ_R_NCM_CONTENT_META_NOT_FOUND;
        return;
    }
    m->used = 0;
}
static void meta_not_found_out(IpcObject *s, IpcCall *c) {
    (void)s;
    c->result = HZ_R_NCM_CONTENT_NOT_FOUND;
} /* no content infos are parsed from meta data */
static void meta_has(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = meta_find(in_key(c, 0)) != NULL;
    IPC_OUT(c, v);
}
static void meta_has_all(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_in_buf(c, 0);
    uint8_t v = 1;
    if (b) {
        const uint8_t *p = ipc_buf_ptr(b);
        for (uint64_t o = 0; o + NCM_KEY_SIZE <= b->size; o += NCM_KEY_SIZE)
            if (!meta_find(p + o)) v = 0;
    }
    IPC_OUT(c, v);
}
/* Writes the keys matching `want(key)` starting at `offset` into output buffer 0; answers {total matching, written}. */
static void list_keys(IpcCall *c, int32_t offset, int (*want)(const uint8_t *key, uint64_t arg), uint64_t arg) {
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint64_t cap = b ? b->size / NCM_KEY_SIZE : 0;
    uint8_t out[NCM_MAX_METAS * NCM_KEY_SIZE];
    int32_t total = 0, written = 0, seen = 0;
    for (int i = 0; i < NCM_MAX_METAS; i++) {
        if (!metas[i].used || !want(metas[i].key, arg)) continue;
        total++;
        if (seen++ < offset) continue;
        if ((uint64_t)written < cap) memcpy(out + (size_t)written++ * NCM_KEY_SIZE, metas[i].key, NCM_KEY_SIZE);
    }
    if (written) ipc_write_buf(c, b, out, (uint64_t)written * NCM_KEY_SIZE);
    int32_t o[2] = {total, written};
    ipc_out(c, o, 8);
}
static int want_all(const uint8_t *k, uint64_t a) {
    (void)k;
    (void)a;
    return 1;
}
static int want_type(const uint8_t *k, uint64_t a) { return k[12] == (uint8_t)a; }
/* List(offset s32, type u8, id u64 filter, ...): a type of 0 lists every type. Layout unverified. */
static void meta_list(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t off = c->in_size >= 4 ? IPC_IN(c, int32_t, 0) : 0;
    uint8_t type = c->in_size >= 5 ? IPC_IN(c, uint8_t, 4) : 0;
    list_keys(c, off, type ? want_type : want_all, type);
}
static void meta_list_application(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t off = c->in_size >= 4 ? IPC_IN(c, int32_t, 0) : 0;
    list_keys(c, off, want_type, NCM_TYPE_APPLICATION);
}
static void meta_list_zero(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t v = 0;
    IPC_OUT(c, v);
}
static void meta_list_info_zero(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t o[2] = {0, 0};
    ipc_out(c, o, 8);
}
static void meta_latest(IpcObject *s, IpcCall *c) {
    (void)s;
    uint64_t id = IPC_IN(c, uint64_t, 0);
    const uint8_t *best = NULL;
    for (int i = 0; i < NCM_MAX_METAS; i++)
        if (metas[i].used && key_id(metas[i].key) == id && (!best || key_version(metas[i].key) > key_version(best))) best = metas[i].key;
    if (!best) {
        c->result = HZ_R_NCM_CONTENT_META_NOT_FOUND;
        return;
    }
    ipc_out(c, best, NCM_KEY_SIZE);
}
static void meta_size(IpcObject *s, IpcCall *c) {
    (void)s;
    NcmMeta *m = meta_find(in_key(c, 0));
    if (!m) {
        c->result = HZ_R_NCM_CONTENT_META_NOT_FOUND;
        return;
    }
    uint64_t n = m->size;
    IPC_OUT(c, n);
}
static void meta_required_system_version(IpcObject *s, IpcCall *c) {
    (void)s;
    if (!meta_find(in_key(c, 0))) {
        c->result = HZ_R_NCM_CONTENT_META_NOT_FOUND;
        return;
    }
    uint32_t v = 0;
    IPC_OUT(c, v);
}
static void meta_patch_id(IpcObject *s, IpcCall *c) {
    (void)s;
    NcmMeta *m = meta_find(in_key(c, 0));
    if (!m) {
        c->result = HZ_R_NCM_CONTENT_META_NOT_FOUND;
        return;
    }
    uint64_t id = key_id(m->key);
    if (m->key[12] == NCM_TYPE_APPLICATION) id |= 0x800; /* the update title id of an application */
    IPC_OUT(c, id);
}
static void meta_has_content(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = 0;
    IPC_OUT(c, v);
}
static void meta_attributes(IpcObject *s, IpcCall *c) {
    (void)s;
    if (!meta_find(in_key(c, 0))) {
        c->result = HZ_R_NCM_CONTENT_META_NOT_FOUND;
        return;
    }
    uint8_t v = 0;
    IPC_OUT(c, v);
}

/* ---- IContentStorage ---- */
static void st_generate_placeholder_id(IpcObject *s, IpcCall *c) {
    (void)s;
    static uint64_t counter;
    uint8_t id[NCM_ID_SIZE] = {0};
    uint64_t n = ++counter;
    memcpy(id, &n, 8);
    id[15] = 0x50;
    ipc_out(c, id, NCM_ID_SIZE);
}
static void st_create_placeholder(IpcObject *s, IpcCall *c) { /* {ContentId, PlaceHolderId, u64 size}: the placeholder id is the second 16 bytes */
    (void)s;
    const uint8_t *id = in_id(c, 16);
    uint64_t size = IPC_IN(c, uint64_t, 32);
    if (!content_add(id, 1, size)) c->result = HZ_R_NCM_CONTENT_NOT_FOUND;
}
static void st_delete_placeholder(IpcObject *s, IpcCall *c) {
    (void)s;
    NcmContent *e = content_find(in_id(c, 0), 1);
    if (!e) {
        c->result = HZ_R_NCM_CONTENT_NOT_FOUND;
        return;
    }
    e->used = 0;
}
static void st_has_placeholder(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = content_find(in_id(c, 0), 1) != NULL;
    IPC_OUT(c, v);
}
static void st_register(IpcObject *s, IpcCall *c) { /* Register(PlaceHolderId, ContentId): the placeholder becomes the content */
    (void)s;
    NcmContent *p = content_find(in_id(c, 0), 1);
    if (!p) {
        c->result = HZ_R_NCM_CONTENT_NOT_FOUND;
        return;
    }
    uint64_t size = p->size;
    p->used = 0;
    if (!content_add(in_id(c, 16), 0, size)) c->result = HZ_R_NCM_CONTENT_NOT_FOUND;
}
static void st_delete(IpcObject *s, IpcCall *c) {
    (void)s;
    NcmContent *e = content_find(in_id(c, 0), 0);
    if (!e) {
        c->result = HZ_R_NCM_CONTENT_NOT_FOUND;
        return;
    }
    e->used = 0;
}
static void st_has(IpcObject *s, IpcCall *c) {
    (void)s;
    uint8_t v = content_find(in_id(c, 0), 0) != NULL;
    IPC_OUT(c, v);
}
static void st_get_path(IpcObject *s, IpcCall *c) { /* registered content has no file on the host: an empty path, not an error */
    (void)s;
    NcmContent *e = content_find(in_id(c, 0), 0);
    if (!e) {
        c->result = HZ_R_NCM_CONTENT_NOT_FOUND;
        return;
    }
    uint8_t path[NCM_PATH_SIZE] = {0};
    ipc_write_buf(c, ipc_out_buf(c, 0), path, sizeof path);
}
static void st_get_placeholder_path(IpcObject *s, IpcCall *c) {
    (void)s;
    NcmContent *e = content_find(in_id(c, 0), 1);
    if (!e) {
        c->result = HZ_R_NCM_CONTENT_NOT_FOUND;
        return;
    }
    uint8_t path[NCM_PATH_SIZE] = {0};
    ipc_write_buf(c, ipc_out_buf(c, 0), path, sizeof path);
}
static void st_cleanup_placeholders(IpcObject *s, IpcCall *c) {
    (void)s;
    (void)c;
    for (int i = 0; i < NCM_MAX_CONTENTS; i++)
        if (contents[i].placeholder) contents[i].used = 0;
}
static void st_list_placeholder(IpcObject *s, IpcCall *c) {
    (void)s;
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint64_t cap = b ? b->size / NCM_ID_SIZE : 0;
    int32_t n = 0;
    uint8_t out[NCM_MAX_CONTENTS * NCM_ID_SIZE];
    for (int i = 0; i < NCM_MAX_CONTENTS; i++)
        if (contents[i].used && contents[i].placeholder) {
            if ((uint64_t)n >= cap) break;
            memcpy(out + (size_t)n++ * NCM_ID_SIZE, contents[i].id, NCM_ID_SIZE);
        }
    if (n) ipc_write_buf(c, b, out, (uint64_t)n * NCM_ID_SIZE);
    IPC_OUT(c, n);
}
static void st_content_count(IpcObject *s, IpcCall *c) {
    (void)s;
    int32_t n = 0;
    for (int i = 0; i < NCM_MAX_CONTENTS; i++)
        if (contents[i].used && !contents[i].placeholder) n++;
    IPC_OUT(c, n);
}
static void st_list_content_id(IpcObject *s, IpcCall *c) { /* ListContentId(offset s32) -> written s32 */
    (void)s;
    int32_t off = c->in_size >= 4 ? IPC_IN(c, int32_t, 0) : 0;
    const IpcBuf *b = ipc_out_buf(c, 0);
    uint64_t cap = b ? b->size / NCM_ID_SIZE : 0;
    int32_t seen = 0, n = 0;
    uint8_t out[NCM_MAX_CONTENTS * NCM_ID_SIZE];
    for (int i = 0; i < NCM_MAX_CONTENTS; i++)
        if (contents[i].used && !contents[i].placeholder) {
            if (seen++ < off) continue;
            if ((uint64_t)n >= cap) break;
            memcpy(out + (size_t)n++ * NCM_ID_SIZE, contents[i].id, NCM_ID_SIZE);
        }
    if (n) ipc_write_buf(c, b, out, (uint64_t)n * NCM_ID_SIZE);
    IPC_OUT(c, n);
}
static void st_size(IpcObject *s, IpcCall *c) {
    (void)s;
    NcmContent *e = content_find(in_id(c, 0), 0);
    if (!e) {
        c->result = HZ_R_NCM_CONTENT_NOT_FOUND;
        return;
    }
    uint64_t n = e->size;
    IPC_OUT(c, n);
}

/* Rows 3, 4, 16 and 17 stay stubs: they would read the content infos out of the stored meta blobs (a CNMT parse), which nothing needs yet (class a, left for
 * a later pass). LookupOrphanContent has no orphans to report and GetPath / GetPlaceHolderPath are complete (registered content has no host file path). */
static const IpcCmd meta_db_cmds[] = {
    {0, "Set", meta_set, IPC_IMPL},
    {1, "Get", meta_get, IPC_IMPL},
    {2, "Remove", meta_remove, IPC_IMPL},
    {3, "GetContentIdByType", meta_not_found_out, IPC_STUB},
    {4, "ListContentInfo", meta_list_zero, IPC_STUB},
    {5, "List", meta_list, IPC_IMPL},
    {6, "GetLatestContentMetaKey", meta_latest, IPC_IMPL},
    {7, "ListApplication", meta_list_application, IPC_IMPL},
    {8, "Has", meta_has, IPC_IMPL},
    {9, "HasAll", meta_has_all, IPC_IMPL},
    {10, "GetSize", meta_size, IPC_IMPL},
    {11, "GetRequiredSystemVersion", meta_required_system_version, IPC_IMPL},
    {12, "GetPatchId", meta_patch_id, IPC_IMPL},
    {13, "DisableForcibly", svc_noop, IPC_IMPL},
    {14, "LookupOrphanContent", svc_noop, IPC_IMPL},
    {15, "Commit", svc_noop, IPC_IMPL},
    {16, "HasContent", meta_has_content, IPC_STUB},
    {17, "ListContentMetaInfo", meta_list_info_zero, IPC_STUB},
    {18, "GetAttributes", meta_attributes, IPC_IMPL},
};
static const IpcCmd storage_cmds[] = {
    {0, "GeneratePlaceHolderId", st_generate_placeholder_id, IPC_IMPL},
    {1, "CreatePlaceHolder", st_create_placeholder, IPC_IMPL},
    {2, "DeletePlaceHolder", st_delete_placeholder, IPC_IMPL},
    {3, "HasPlaceHolder", st_has_placeholder, IPC_IMPL},
    {5, "Register", st_register, IPC_IMPL},
    {6, "Delete", st_delete, IPC_IMPL},
    {7, "Has", st_has, IPC_IMPL},
    {8, "GetPath", st_get_path, IPC_IMPL},
    {9, "GetPlaceHolderPath", st_get_placeholder_path, IPC_IMPL},
    {10, "CleanupAllPlaceHolder", st_cleanup_placeholders, IPC_IMPL},
    {11, "ListPlaceHolder", st_list_placeholder, IPC_IMPL},
    {12, "GetContentCount", st_content_count, IPC_IMPL},
    {13, "ListContentId", st_list_content_id, IPC_IMPL},
    {14, "GetSizeFromContentId", st_size, IPC_IMPL},
};
static void ncm_open_storage(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IContentStorage", storage_cmds, NULL, NULL));
}
static void ncm_open_meta_db(IpcObject *s, IpcCall *c) {
    (void)s;
    ipc_out_object(c, IPC_OBJECT("IContentMetaDatabase", meta_db_cmds, NULL, NULL));
}
static const IpcCmd ncm_cmds[] = {
    {0, "CreateContentStorage", svc_noop, IPC_IMPL},           {1, "CreateContentMetaDatabase", svc_noop, IPC_IMPL},
    {2, "VerifyContentStorage", svc_noop, IPC_IMPL},           {3, "VerifyContentMetaDatabase", svc_noop, IPC_IMPL},
    {4, "OpenContentStorage", ncm_open_storage, IPC_IMPL},     {5, "OpenContentMetaDatabase", ncm_open_meta_db, IPC_IMPL},
    {6, "CloseContentStorageForcibly", svc_noop, IPC_IMPL},    {7, "CloseContentMetaDatabaseForcibly", svc_noop, IPC_IMPL},
    {8, "CleanupContentMetaDatabase", svc_noop, IPC_IMPL},     {9, "ActivateContentStorage", svc_noop, IPC_IMPL},
    {10, "InactivateContentStorage", svc_noop, IPC_IMPL},      {11, "ActivateContentMetaDatabase", svc_noop, IPC_IMPL},
    {12, "InactivateContentMetaDatabase", svc_noop, IPC_IMPL}, {13, "InvalidateRightsIdCache", svc_noop, IPC_IMPL},
};
static IpcObject *ncm_open(void) { return IPC_OBJECT("ncm", ncm_cmds, NULL, NULL); }
void service_register_ncm(void) { ipc_register_service("ncm", ncm_open); }
