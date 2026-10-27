/* Unit tests: Mii database core and mii:u IPC table. host_info.c is not linked on Linux: host_user_name is provided here. */
#include "../src/services/mii.c"
#include "svc_test.h"
#include <unistd.h>
#include <sys/wait.h>
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

    /* real default Miis: the six entries of the firmware's mii service (sdb) */
    static const uint8_t hair[6] = {68, 55, 33, 24, 14, 12}, fav[6] = {4, 5, 0, 2, 6, 7};
    for (uint32_t i = 0; i < 6; i++) {
        mii_build_default(i, &m);
        CHECK(m.hair_type == hair[i] && m.favorite_color == fav[i] && m.gender == (i >= 3), "default Mii %u: hair %u, favourite colour %u, gender as read from the firmware", i, hair[i], fav[i]);
    }
    mii_build_default(5, &m); CHECK(m.hair_color == 1 && m.eye_color == 8 && m.mouth_color == 19 && m.glasses_color == 8, "default colours go through the service's 3DS-to-common tables");

    /* CRC and conversions */
    CHECK(mii_crc16("123456789", 9) == 0x31C3, "CRC-16/XMODEM check value");
    for (uint32_t i = 0; i < 40; i++) {
        MiiCharInfo a, b; MiiCoreData core; MiiStoreData st;
        if (i < 6) mii_build_default(i, &a); else mii_build_random(0, i % 3, 0, &a);
        CHECK(mii_char_info_valid(&a), "Mii %u is valid", i);
        mii_core_from_char_info(&a, &core); mii_char_info_from_core(&core, a.create_id, &b);
        CHECK(!memcmp(&a, &b, sizeof a) || (a.padding == 0 && !memcmp(&a, &b, sizeof a - 1)), "CharInfo -> CoreData -> CharInfo is lossless (Mii %u)", i);
        mii_store_from_char_info(&a, &st); CHECK(mii_store_verify(&st), "StoreData of Mii %u verifies", i);
        st.core.data[3] ^= 1; CHECK(!mii_store_verify(&st), "a flipped bit breaks the CRC");
    }
    { MiiCharInfo r1, r2; mii_build_random(0, MII_GENDER_FEMALE, 0, &r1); mii_build_random(0, MII_GENDER_FEMALE, 0, &r2);
      CHECK(r1.gender == 1 && memcmp(r1.create_id, r2.create_id, 16) && memcmp(&r1, &r2, sizeof r1), "BuildRandom: fresh id and different faces"); }

    /* the database: persisted, edited, reloaded */
    char path[128]; snprintf(path, sizeof path, "/tmp/napple-mii-test-%d.dat", (int)getpid()); remove(path);
    mii_db_set_path(path);
    CHECK(mii_db_count() == 1, "a new database holds the player's Mii"); { MiiStoreData u; mii_db_get(0, &u); MiiCharInfo ui; mii_char_info_from_store(&u, &ui); CHECK(ui.name[0] == (uint16_t)(uint8_t)host_user_name()[0], "named after the host user"); }
    MiiCharInfo e1, e2; mii_build_random(0, 0, 0, &e1); mii_build_random(0, 1, 0, &e2);
    MiiStoreData s1, s2; mii_store_from_char_info(&e1, &s1); mii_store_from_char_info(&e2, &s2);
    CHECK(mii_db_add_or_replace(&s1) == MII_OK && mii_db_add_or_replace(&s2) == MII_OK && mii_db_count() == 3, "two Miis added");
    uint32_t gen = mii_db_generation(); s1.core.data[0] ^= 0; e1.hair_type = (uint8_t)((e1.hair_type + 1) % 100); mii_store_from_char_info(&e1, &s1);
    CHECK(mii_db_add_or_replace(&s1) == MII_OK && mii_db_count() == 3 && mii_db_generation() != gen, "same create id: replaced, not appended");
    CHECK(mii_db_move(e2.create_id, 0) == MII_OK && mii_db_find(e2.create_id) == 0 && mii_db_find(e1.create_id) == 2, "move to the front");
    mii_db_set_path(NULL); mii_db_set_path(path);           /* a restart */
    CHECK(mii_db_count() == 3 && mii_db_find(e2.create_id) == 0, "the database survives a restart");
    { MiiStoreData g; mii_db_get(2, &g); MiiCharInfo gi; mii_char_info_from_store(&g, &gi); CHECK(gi.hair_type == e1.hair_type, "the edit survived the restart"); }
    CHECK(mii_db_delete(e2.create_id) == MII_OK && mii_db_delete(e2.create_id) == MII_ERR_NOT_FOUND && mii_db_count() == 2, "delete");
    { uint8_t x[16 + 100 * 0x44]; size_t xn = 0; CHECK(mii_db_export(x, sizeof x, &xn) == MII_OK && xn == 16 + 2 * 0x44, "export");
      mii_db_format(); CHECK(mii_db_count() == 0 && mii_count(MII_FLAG_DATABASE) == 0, "format empties it");
      CHECK(mii_db_import(x, xn) == MII_OK && mii_db_count() == 2, "import restores it"); x[20] ^= 0xff; CHECK(mii_db_import(x, xn) == MII_ERR_INVALID && mii_db_count() == 2, "a damaged export is refused"); }
    for (int i = 0; i < 120; i++) { MiiCharInfo r; mii_build_random(0, 0, 0, &r); MiiStoreData st; mii_store_from_char_info(&r, &st); int rc = mii_db_add_or_replace(&st); if (i >= 98) CHECK(rc == MII_ERR_FULL, "the 101st Mii is refused"); }
    CHECK(mii_db_count() == MII_DB_MAX, "full at 100");
    { FILE *f = fopen(path, "r+b"); fseek(f, 40, SEEK_SET); fputc(0x5a, f); fputc(0xa5, f); fclose(f); }   /* damage the file */
    mii_db_set_path(NULL); mii_db_set_path(path);
    CHECK(mii_db_count() == 99 && mii_db_take_broken() == 1 && mii_db_take_broken() == 0, "a damaged file falls back to the previous copy and is reported once");
    { char bak[160]; snprintf(bak, sizeof bak, "%s.bak", path); FILE *f = fopen(path, "wb"); fputs("garbage", f); fclose(f); f = fopen(bak, "wb"); fputs("junk", f); fclose(f); }
    mii_db_set_path(NULL); mii_db_set_path(path);
    CHECK(mii_db_count() == 1 && mii_db_take_broken() == 1, "file and backup both damaged: the seed, no crash");
    remove(path);

    /* two processes at once: each adds Miis in a loop; no edit is lost and the file always parses */
    { char p2[160]; snprintf(p2, sizeof p2, "/tmp/napple-mii-conc-%d.dat", (int)getpid()); remove(p2); mii_db_set_path(p2);
      (void)mii_db_count();
      pid_t kid = fork();
      if (kid == 0) { mii_db_set_path(p2); for (int i = 0; i < 20; i++) { MiiCharInfo r; mii_build_random(0, 0, 0, &r); MiiStoreData st; mii_store_from_char_info(&r, &st); mii_db_add_or_replace(&st); } _exit(0); }
      for (int i = 0; i < 20; i++) { MiiCharInfo r; mii_build_random(0, 1, 0, &r); MiiStoreData st; mii_store_from_char_info(&r, &st); mii_db_add_or_replace(&st); }
      int status; waitpid(kid, &status, 0);
      CHECK(mii_db_count() == 41, "two processes, 20 edits each: all 40 Miis plus the player's survive (%u)", mii_db_count());
      uint8_t x[16 + 100 * 0x44]; size_t xn; CHECK(mii_db_export(x, sizeof x, &xn) == MII_OK && xn == 16 + 41 * 0x44, "the shared file parses");
      remove(p2); char b2[170]; snprintf(b2, sizeof b2, "%s.bak", p2); remove(b2); snprintf(b2, sizeof b2, "%s.lock", p2); remove(b2); }

    /* IPC: mii:e, mii:u share the database */
    snprintf(path, sizeof path, "/tmp/napple-mii-ipc-%d.dat", (int)getpid()); remove(path); mii_db_set_path(path);
    { IpcObject *eroot = mii_e_open(); IpcCall ec; svc_call(eroot, 0, "\0\0\0\0", 4, &ec); IpcObject *edb = ec.out_objects[0];
      IpcObject *uroot = mii_open(); IpcCall uc; svc_call(uroot, 0, "\0\0\0\0", 4, &uc); IpcObject *udb = uc.out_objects[0];
      CHECK(svc_find(edb, 13) && svc_find(edb, 14) && !svc_find(udb, 13) && !svc_find(udb, 14), "mii:u has no editor commands");
      MiiCharInfo n1; mii_build_random(0, 1, 0, &n1); IpcCall c2;
      svc_call(udb, 0, &(uint32_t){1}, 4, &c2); CHECK(c2.out[0] == 0, "IsUpdated: nothing changed yet");
      svc_call(edb, 26, &n1, sizeof n1, &c2); CHECK(c2.result == 0, "Append (mii:e)");
      uint32_t flag = 1; svc_call(udb, 0, &flag, 4, &c2); CHECK(c2.out[0] == 1, "IsUpdated: the editor's change is seen by the mii:u session");
      svc_call(udb, 2, &flag, 4, &c2); CHECK(SVC_OUT(&c2, int32_t, 0) == 2, "GetCount sees the new Mii");
      MiiStoreData ns; mii_store_from_char_info(&n1, &ns); n1.hair_type = 3; MiiStoreData ns2; mii_store_from_char_info(&n1, &ns2);
      svc_call(edb, 13, &ns2, sizeof ns2, &c2); CHECK(c2.result == 0, "AddOrReplace replaces");
      uint8_t mv[20]; int32_t idx = 0; memcpy(mv, &idx, 4); memcpy(mv + 4, n1.create_id, 16); svc_call(edb, 12, mv, 20, &c2); CHECK(c2.result == 0 && mii_db_find(n1.create_id) == 0, "Move");
      static uint8_t sbuf[0x48 * 8]; IpcCall g; memset(&g, 0, sizeof g); g.in = (const uint8_t *)&flag; g.in_size = 4; svc_set_out_buf(&g, sbuf, sizeof sbuf); svc_call_c(udb, 8, &g);
      CHECK(SVC_OUT(&g, int32_t, 0) == 2 && mii_store_verify(&((MiiStoreElement *)sbuf)[0].store) && ((MiiStoreElement *)sbuf)[0].source == MII_SOURCE_DATABASE, "Get2: StoreData elements (0x48) that verify");
      MiiCharInfo cc; mii_build_default(1, &cc); MiiCoreData core; svc_call(udb, 25, &cc, sizeof cc, &c2); memcpy(&core, c2.out, sizeof core);
      svc_call(udb, 24, &core, sizeof core, &c2); CHECK(((MiiCharInfo *)c2.out)->hair_type == cc.hair_type && ((MiiCharInfo *)c2.out)->eye_type == cc.eye_type, "ConvertCharInfoToCoreData / ConvertCoreDataToCharInfo");
      svc_call(edb, 14, n1.create_id, 16, &c2); CHECK(c2.result == 0 && mii_db_count() == 1, "Delete");
      svc_call(edb, 14, n1.create_id, 16, &c2); CHECK(c2.result != 0, "Delete of an unknown id fails");
      uint32_t src2 = 3; MiiCharInfo wr; mii_build_default(2, &wr); uint8_t ul[sizeof wr + 4]; memcpy(ul, &wr, sizeof wr); memcpy(ul + sizeof wr, &src2, 4);
      svc_call(udb, 5, ul, sizeof ul, &c2); CHECK(!memcmp(c2.out, &wr, sizeof wr), "UpdateLatest of a default Mii returns the default"); }
    remove(path);
    printf("{\"test\":\"svc_mii\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
