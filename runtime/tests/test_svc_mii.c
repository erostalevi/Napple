/* Unit tests: Mii database core and mii:u IPC table. host_info.c is not linked on Linux: host_user_name is provided here. */
#include "../src/services/mii.c"
#include "svc_test.h"
#ifdef __linux__
const char *host_user_name(void) { return "Tester"; }
#endif

int main(void) {
    MiiCharInfo m;
    for (uint32_t i = 0; i < MII_DEFAULT_COUNT; i++) { mii_build_default(i, &m); CHECK(mii_char_info_valid(&m), "default Mii %u is inside every editor range", i); }
    mii_build_default(0, &m); MiiCharInfo m1; mii_build_default(3, &m1);   /* defaults 0..2 male, 3..5 female */
    CHECK(m.gender != m1.gender && memcmp(m.create_id, m1.create_id, 16) != 0, "defaults differ in gender and id");
    CHECK((m.create_id[6] & 0xf0) == 0x40 && (m.create_id[8] & 0xc0) == 0x80, "default create id is a UUID v4 (CharInfo::Verify)");
    CHECK(m.name[0] == 'n' && m.name[7] == 0, "default name \"no name\" is NUL-terminated UTF-16");
    MiiCharInfo wrap; mii_build_default(MII_DEFAULT_COUNT + 2, &wrap); mii_build_default(2, &m); CHECK(!memcmp(&wrap, &m, sizeof m), "index wraps");
    uint16_t u[8]; size_t n = mii_utf8_to_utf16("A\xC3\xA9\xE2\x82\xAC", u, 8); CHECK(n == 3 && u[0] == 'A' && u[1] == 0xE9 && u[2] == 0x20AC, "UTF-8 to UTF-16 (BMP)");
    CHECK(mii_count(0) == 0 && mii_count(MII_FLAG_DATABASE) == 1 && mii_count(MII_FLAG_DEFAULT) == 6 && mii_count(3) == 7, "counts by source flag");
    MiiElement el[8]; uint32_t got = mii_list(3, el, 8);
    CHECK(got == 7 && el[0].source == MII_SOURCE_DATABASE && el[1].source == MII_SOURCE_DEFAULT, "list: database first, then defaults");
    CHECK(mii_list(3, el, 3) == 3, "list respects capacity");
    CHECK(mii_find_index(el[0].info.create_id) == 0 && mii_find_index(el[1].info.create_id) == -1, "find index");

    /* IPC */
    IpcObject *root = mii_open(); IpcCall c;
    svc_call(root, 0, "\0\0\0\0", 4, &c); CHECK(c.nout_objects == 1, "GetDatabaseService returns the database object");
    IpcObject *db = c.out_objects[0];
    uint32_t src = 3; svc_call(db, 2, &src, 4, &c); CHECK(c.out_size == 4 && SVC_OUT(&c, int32_t, 0) == 7, "GetCount(Database|Default) = 7");
    src = 1; svc_call(db, 2, &src, 4, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 1, "GetCount(Database) = 1");
    static uint8_t buf[0x5c * 8];
    src = 3; memset(&c, 0, sizeof c); c.in = (const uint8_t *)&src; c.in_size = 4; svc_set_out_buf(&c, buf, sizeof buf); svc_call_c(db, 3, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == 7, "Get: 7 elements written");
    MiiElement *e = (MiiElement *)buf;
    CHECK(e[0].source == MII_SOURCE_DATABASE && e[0].info.name[0] == (uint16_t)(uint8_t)host_user_name()[0] && e[1].source == MII_SOURCE_DEFAULT, "Get: elements carry the source word and the host user's name");
    memset(buf, 0, sizeof buf); memset(&c, 0, sizeof c); c.in = (const uint8_t *)&src; c.in_size = 4; svc_set_out_buf(&c, buf, 0x58 * 2 + 5); svc_call_c(db, 4, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == 2 && ((MiiCharInfo *)buf)[1].create_id[9] != 0, "Get1: capacity of two CharInfo entries, plain 0x58 stride");
    memset(&c, 0, sizeof c); c.in = (const uint8_t *)&src; c.in_size = 4; svc_call_c(db, 3, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "Get with no output buffer writes nothing");
    uint32_t idx = 4; svc_call(db, 7, &idx, 4, &c); CHECK(c.out_size == 0x58 && mii_char_info_valid((MiiCharInfo *)c.out), "BuildDefault returns a valid CharInfo");
    uint32_t rnd[3] = {0, MII_GENDER_FEMALE, 0}; svc_call(db, 6, rnd, 12, &c); CHECK(((MiiCharInfo *)c.out)->gender == MII_GENDER_FEMALE, "BuildRandom honours gender");
    svc_call(db, 11, e[0].info.create_id, 16, &c); CHECK(c.result == 0 && SVC_OUT(&c, int32_t, 0) == 0, "FindIndex of the user's Mii = 0");
    svc_call(db, 11, e[3].info.create_id, 16, &c); CHECK(c.result != 0, "FindIndex of an unknown id fails");
    svc_call(db, 5, &e[0].info, sizeof(MiiCharInfo), &c); CHECK(c.out_size == 0x58 && !memcmp(c.out, &e[0].info, 0x58), "UpdateLatest echoes");
    svc_call(db, 1, NULL, 0, &c); CHECK(c.out[0] == 0, "database is never full");
    printf("{\"test\":\"svc_mii\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
