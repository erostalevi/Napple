/* Unit tests: ncm keeps a real in-memory content-meta database and content storage. */
#include "../src/services/ncm.c"
#include "svc_test.h"

static void key(uint8_t k[16], uint64_t id, uint32_t ver, uint8_t type) { memset(k, 0, 16); memcpy(k, &id, 8); memcpy(k + 8, &ver, 4); k[12] = type; }

int main(void) {
    IpcObject *o = ncm_open(); IpcCall c;
    svc_call(o, 5, "\x01", 1, &c); CHECK(c.nout_objects == 1, "OpenContentMetaDatabase returns an object");
    IpcObject *db = c.out_objects[0];
    uint8_t k1[16], k2[16], k3[16], none[16];
    key(k1, 0x0100000000001000ull, 0, NCM_TYPE_APPLICATION); key(k2, 0x0100000000001000ull, 0x10000, NCM_TYPE_PATCH);
    key(k3, 0x0100000000001000ull, 0x20000, NCM_TYPE_PATCH); key(none, 0x42, 0, NCM_TYPE_ADDON);

    svc_call(db, 8, k1, 16, &c); CHECK(c.result == 0 && c.out[0] == 0, "Has(key) = false when empty");
    svc_call(db, 5, NULL, 0, &c); CHECK(c.out_size == 8 && SVC_OUT(&c, int32_t, 0) == 0 && SVC_OUT(&c, int32_t, 4) == 0, "List: total 0, written 0");
    svc_call(db, 6, k1, 8, &c); CHECK(c.result == HZ_R_NCM_CONTENT_META_NOT_FOUND && HZ_MODULE(c.result) == HZ_MOD_NCM, "GetLatestContentMetaKey: ContentMetaNotFound");
    svc_call(db, 1, k1, 16, &c); CHECK(c.result == HZ_R_NCM_CONTENT_META_NOT_FOUND, "Get of a missing key");

    uint8_t data[0x20]; memset(data, 0xab, sizeof data);
    IpcCall s = {0};
    s = (IpcCall){0}; s.in = k1; s.in_size = 16; svc_set_in_buf(&s, data, sizeof data); svc_call_c(db, 0, &s); CHECK(s.result == 0, "Set(k1)");
    s = (IpcCall){0}; s.in = k2; s.in_size = 16; svc_set_in_buf(&s, data, 8); svc_call_c(db, 0, &s); CHECK(s.result == 0, "Set(k2)");
    s = (IpcCall){0}; s.in = k3; s.in_size = 16; svc_set_in_buf(&s, data, 4); svc_call_c(db, 0, &s); CHECK(s.result == 0, "Set(k3)");

    svc_call(db, 8, k1, 16, &c); CHECK(c.out[0] == 1, "Has(k1) after Set");
    svc_call(db, 8, none, 16, &c); CHECK(c.out[0] == 0, "Has(other) = false");
    svc_call(db, 10, k1, 16, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == sizeof data, "GetSize = stored size");
    uint8_t back[0x40] = {0}; c = (IpcCall){0}; c.in = k1; c.in_size = 16; svc_set_out_buf(&c, back, sizeof back); svc_call_c(db, 1, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, uint64_t, 0) == sizeof data && back[0] == 0xab && back[0x1f] == 0xab && back[0x20] == 0, "Get returns the stored meta data");

    uint8_t pair[32]; memcpy(pair, k1, 16); memcpy(pair + 16, k2, 16);
    c = (IpcCall){0}; svc_set_in_buf(&c, pair, 32); svc_call_c(db, 9, &c); CHECK(c.out[0] == 1, "HasAll(k1,k2) = true");
    memcpy(pair + 16, none, 16);
    c = (IpcCall){0}; svc_set_in_buf(&c, pair, 32); svc_call_c(db, 9, &c); CHECK(c.out[0] == 0, "HasAll with a missing key = false");

    uint64_t id = 0x0100000000001000ull;
    svc_call(db, 6, &id, 8, &c); CHECK(c.result == 0 && !memcmp(c.out, k3, 16), "GetLatestContentMetaKey picks the highest version");
    svc_call(db, 12, k1, 16, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == (id | 0x800), "GetPatchId of an application = its update id");
    svc_call(db, 11, k1, 16, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 0, "GetRequiredSystemVersion");
    svc_call(db, 11, none, 16, &c); CHECK(c.result == HZ_R_NCM_CONTENT_META_NOT_FOUND, "GetRequiredSystemVersion of a missing key");
    svc_call(db, 18, k1, 16, &c); CHECK(c.result == 0, "GetAttributes");

    uint8_t keys[16 * 4]; c = (IpcCall){0}; svc_set_out_buf(&c, keys, sizeof keys); svc_call_c(db, 5, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == 3 && SVC_OUT(&c, int32_t, 4) == 3 && !memcmp(keys, k1, 16), "List: 3 keys in insertion order");
    uint8_t one[16]; c = (IpcCall){0}; svc_set_out_buf(&c, one, sizeof one); svc_call_c(db, 5, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == 3 && SVC_OUT(&c, int32_t, 4) == 1, "List: small buffer reports total 3, written 1");
    int32_t off = 2; c = (IpcCall){0}; c.in = (const uint8_t *)&off; c.in_size = 4; svc_set_out_buf(&c, keys, sizeof keys); svc_call_c(db, 5, &c);
    CHECK(SVC_OUT(&c, int32_t, 4) == 1 && !memcmp(keys, k3, 16), "List: offset skips entries");
    c = (IpcCall){0}; svc_set_out_buf(&c, keys, sizeof keys); svc_call_c(db, 7, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == 1 && SVC_OUT(&c, int32_t, 4) == 1 && !memcmp(keys, k1, 16), "ListApplication lists only applications");

    svc_call(db, 2, k2, 16, &c); CHECK(c.result == 0, "Remove(k2)");
    svc_call(db, 8, k2, 16, &c); CHECK(c.out[0] == 0, "Has(k2) after Remove");
    svc_call(db, 2, k2, 16, &c); CHECK(c.result == HZ_R_NCM_CONTENT_META_NOT_FOUND, "Remove of a missing key");
    svc_call(db, 15, NULL, 0, &c); CHECK(c.result == 0, "Commit is accepted");

    svc_call(o, 4, "\x01", 1, &c); IpcObject *st = c.out_objects[0];
    uint8_t cid[16], ph[16], in[48] = {0}; memset(cid, 0x11, 16);
    svc_call(st, 0, NULL, 0, &c); CHECK(c.out_size == 16, "GeneratePlaceHolderId returns an id"); memcpy(ph, c.out, 16);
    uint8_t ph2[16]; svc_call(st, 0, NULL, 0, &c); memcpy(ph2, c.out, 16); CHECK(memcmp(ph, ph2, 16) != 0, "placeholder ids are unique");
    svc_call(st, 3, ph, 16, &c); CHECK(c.out[0] == 0, "HasPlaceHolder false before create");
    memcpy(in + 16, ph, 16); uint64_t size = 0x1234; memcpy(in + 32, &size, 8);
    svc_call(st, 1, in, 40, &c); CHECK(c.result == 0, "CreatePlaceHolder");
    svc_call(st, 3, ph, 16, &c); CHECK(c.out[0] == 1, "HasPlaceHolder true after create");
    c = (IpcCall){0}; svc_set_out_buf(&c, keys, sizeof keys); svc_call_c(st, 11, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 1 && !memcmp(keys, ph, 16), "ListPlaceHolder");
    svc_call(st, 7, cid, 16, &c); CHECK(c.out[0] == 0, "storage Has(content id) = false");
    svc_call(st, 8, cid, 16, &c); CHECK(c.result == HZ_R_NCM_CONTENT_NOT_FOUND, "storage GetPath: ContentNotFound");
    uint8_t reg[32]; memcpy(reg, ph, 16); memcpy(reg + 16, cid, 16);
    svc_call(st, 5, reg, 32, &c); CHECK(c.result == 0, "Register");
    svc_call(st, 3, ph, 16, &c); CHECK(c.out[0] == 0, "placeholder consumed by Register");
    svc_call(st, 7, cid, 16, &c); CHECK(c.out[0] == 1, "Has(content id) after Register");
    svc_call(st, 12, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 1, "GetContentCount = 1");
    svc_call(st, 14, cid, 16, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == 0x1234, "GetSizeFromContentId = placeholder size");
    uint8_t path[0x300]; memset(path, 0x7f, sizeof path); c = (IpcCall){0}; c.in = cid; c.in_size = 16; svc_set_out_buf(&c, path, sizeof path); svc_call_c(st, 8, &c);
    CHECK(c.result == 0 && path[0] == 0 && path[0x2ff] == 0, "GetPath answers an empty path for registered content");
    uint8_t ids[32]; c = (IpcCall){0}; svc_set_out_buf(&c, ids, sizeof ids); svc_call_c(st, 13, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 1 && !memcmp(ids, cid, 16), "ListContentId");
    svc_call(st, 6, cid, 16, &c); CHECK(c.result == 0, "Delete");
    svc_call(st, 6, cid, 16, &c); CHECK(c.result == HZ_R_NCM_CONTENT_NOT_FOUND, "Delete of missing content");
    svc_call(st, 12, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "GetContentCount back to 0");
    svc_call(st, 1, in, 40, &c); svc_call(st, 10, NULL, 0, &c);
    svc_call(st, 3, ph, 16, &c); CHECK(c.out[0] == 0, "CleanupAllPlaceHolder drops placeholders");
    CHECK(HZ_DESC(HZ_R_NCM_CONTENT_NOT_FOUND) == 310, "result packing");
    /* a parsed meta blob: header + 4-byte ext header + 2 contents (program, control) + 1 meta info */
    {
        uint8_t blob[8 + 4 + 2 * 0x18 + 0x10] = {0}, kp[16] = {0}, id1[16], id2[16];
        blob[0] = 4; blob[2] = 2; blob[4] = 1; blob[6] = 1; /* ext size, content count, meta count, attributes = 1 */
        for (int i = 0; i < 16; i++) { id1[i] = (uint8_t)(0x10 + i); id2[i] = (uint8_t)(0x40 + i); }
        memcpy(blob + 12, id1, 16); blob[12 + 23] = 1; /* type program */
        memcpy(blob + 12 + 0x18, id2, 16); blob[12 + 0x18 + 23] = 3; /* type control */
        blob[12 + 0x30] = 0x77; /* meta info id byte 0 */
        uint64_t pid = 0x0100000000002000ull; memcpy(kp, &pid, 8); kp[12] = NCM_TYPE_APPLICATION;
        s = (IpcCall){0}; s.in = kp; s.in_size = 16; svc_set_in_buf(&s, blob, sizeof blob); svc_call_c(db, 0, &s); CHECK(s.result == 0, "Set(parsed meta)");
        uint8_t in[24] = {0}; in[0] = 1; memcpy(in + 8, kp, 16);
        svc_call(db, 3, in, 24, &c); CHECK(c.result == 0 && !memcmp(c.out, id1, 16), "GetContentIdByType(program)");
        in[0] = 3; svc_call(db, 3, in, 24, &c); CHECK(c.result == 0 && !memcmp(c.out, id2, 16), "GetContentIdByType(control)");
        in[0] = 5; svc_call(db, 3, in, 24, &c); CHECK(c.result == HZ_R_NCM_CONTENT_NOT_FOUND, "GetContentIdByType: type not present");
        memcpy(in + 8, none, 16); svc_call(db, 3, in, 24, &c); CHECK(c.result == HZ_R_NCM_CONTENT_META_NOT_FOUND, "GetContentIdByType: no such meta");
        memset(in, 0, sizeof in); memcpy(in + 8, kp, 16);
        uint8_t infos[0x60] = {0}; c = (IpcCall){0}; c.in = in; c.in_size = 24; svc_set_out_buf(&c, infos, sizeof infos); svc_call_c(db, 4, &c);
        CHECK(c.result == 0 && SVC_OUT(&c, int32_t, 0) == 2 && !memcmp(infos, id1, 16) && !memcmp(infos + 0x18, id2, 16), "ListContentInfo lists both contents");
        in[0] = 1; memset(infos, 0, sizeof infos); c = (IpcCall){0}; c.in = in; c.in_size = 24; svc_set_out_buf(&c, infos, sizeof infos); svc_call_c(db, 4, &c);
        CHECK(SVC_OUT(&c, int32_t, 0) == 1 && !memcmp(infos, id2, 16), "ListContentInfo honours the offset");
        in[0] = 0; c = (IpcCall){0}; c.in = in; c.in_size = 24; svc_set_out_buf(&c, infos, 0x18); svc_call_c(db, 4, &c);
        CHECK(SVC_OUT(&c, int32_t, 0) == 1, "ListContentInfo is cut to the buffer");
        c = (IpcCall){0}; c.in = in; c.in_size = 24; svc_set_out_buf(&c, infos, sizeof infos); svc_call_c(db, 17, &c);
        CHECK(SVC_OUT(&c, int32_t, 0) == 1 && infos[0] == 0x77, "ListContentMetaInfo returns the meta info record");
        uint8_t hc[32]; memcpy(hc, kp, 16); memcpy(hc + 16, id2, 16);
        svc_call(db, 16, hc, 32, &c); CHECK(c.out[0] == 1, "HasContent(true)");
        id2[0] ^= 1; memcpy(hc + 16, id2, 16); svc_call(db, 16, hc, 32, &c); CHECK(c.out[0] == 0, "HasContent(false)");
        svc_call(db, 18, kp, 16, &c); CHECK(c.out[0] == 1, "GetAttributes reads the header byte");
        svc_call(db, 16, k1, 16, &c); CHECK(c.result == 0 && c.out[0] == 0, "HasContent on an unparseable blob = false");
        in[0] = 0; memcpy(in + 8, k1, 16); c = (IpcCall){0}; c.in = in; c.in_size = 24; svc_set_out_buf(&c, infos, sizeof infos); svc_call_c(db, 4, &c);
        CHECK(c.result == 0 && SVC_OUT(&c, int32_t, 0) == 0, "ListContentInfo on an unparseable blob = 0");
    }
    printf("{\"test\":\"svc_ncm\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
