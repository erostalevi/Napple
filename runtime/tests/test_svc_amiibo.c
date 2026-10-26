/* Unit tests: the amiibo model (virtual amiibo, JSON, store), the dump crypto with a generated test-only key, the metadata library and the picker.
 * Everything here is synthetic: the "keys" are derived in the test, the amiibo ids and names are invented. */
#include "../src/services/amiibo_lib_core.h"
#include "../src/services/hz_result.h"
#include "check.h"
#include <sys/stat.h>
#include <unistd.h>

static void hex(const uint8_t *p, size_t n, char *out) { for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", p[i]); }
static int unhex(const char *s, uint8_t *out, size_t n) { for (size_t i = 0; i < n; i++) { unsigned v; if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1; out[i] = (uint8_t)v; } return 0; }
static void test_keys(AmiiboKeys *k, uint8_t salt) { /* NOT a real key: derived from a counter */
    uint8_t seed[2] = {salt, 0}, h[32];
    for (int set = 0; set < 2; set++) {
        uint8_t *m = set ? k->tag : k->data;
        for (int i = 0; i < 80; i += 32) { seed[1] = (uint8_t)(set * 10 + i); ami_sha256(seed, 2, h); memcpy(m + i, h, 80 - i < 32 ? 80 - i : 32); }
        memcpy(m + 16, set ? "locked secret\0" : "unfixed infos\0", 14); /* type string */
        m[30] = 0; m[31] = set ? 16 : 14; /* rfu, magic size */
    }
}
static int fired;
static void on_change(void *x) { (void)x; fired++; }
static uint8_t ID_A[8] = {0x01, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x02}, ID_B[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x01, 0x02};
static const char API_DOC[] =
    "{\"amiibo\":[{\"amiiboSeries\":\"Test Series\",\"character\":\"Zed\",\"gameSeries\":\"Test Game\",\"head\":\"01010000\",\"image\":\"https://example.invalid/i.png\",\"name\":\"Zed Figure\","
    "\"release\":{\"au\":\"2020-01-01\",\"eu\":null},\"tail\":\"00040002\",\"type\":\"Figure\"},"
    "{\"amiiboSeries\":\"Other\",\"character\":\"Alpha\",\"gameSeries\":\"Test Game\",\"head\":\"00000000\",\"image\":\"x\",\"name\":\"alpha card\",\"release\":{},\"tail\":\"00090102\",\"type\":\"Card\"},"
    "{\"head\":\"bad\",\"tail\":\"0\",\"name\":\"broken entry is skipped\"}]}";
static const char DB_DOC[] = "{\"0x0000000000000002\":{\"name\":\"Db One\",\"release\":{\"na\":\"x\"}},\"0x01010000000400AA\":{\"name\":\"db two\"},\"junk\":{\"name\":\"no\"}}";

int main(void) {
    char tmpl[] = "/tmp/napple-amiibo-test-XXXXXX"; char *root = mkdtemp(tmpl); CHECK(root != NULL, "temp dir");
    char store[256], cache[256]; snprintf(store, sizeof store, "%s/store", root); snprintf(cache, sizeof cache, "%s/cache/amiibo.json", root);

    /* ---- primitives against published vectors ---- */
    uint8_t d[32]; char h[80];
    ami_sha256("abc", 3, d); hex(d, 32, h); CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256(abc)");
    ami_sha256("", 0, d); hex(d, 32, h); CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "SHA-256(empty)");
    { uint8_t big[1000]; memset(big, 'a', sizeof big); ami_sha256(big, sizeof big, d); hex(d, 32, h); CHECK(!strcmp(h, "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"), "SHA-256(1000 x 'a'), multi-block"); }
    { uint8_t key[20]; memset(key, 0x0b, 20); ami_hmac_sha256(key, 20, "Hi There", 8, d); hex(d, 32, h); CHECK(!strcmp(h, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"), "HMAC-SHA256 RFC 4231 case 1"); }
    { ami_hmac_sha256((const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28, d); hex(d, 32, h); CHECK(!strcmp(h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "HMAC-SHA256 RFC 4231 case 2"); }
    { uint8_t key[16], in[16], out[16]; for (int i = 0; i < 16; i++) key[i] = (uint8_t)i, in[i] = (uint8_t)(i * 0x11); ami_aes128_encrypt_block(key, in, out); hex(out, 16, h); CHECK(!strcmp(h, "69c4e0d86a7b0430d8cdb78070b4c55a"), "AES-128 FIPS-197 block"); }
    { uint8_t key[16], ctr[16], pt[32], ct[32]; unhex("2b7e151628aed2a6abf7158809cf4f3c", key, 16); unhex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", ctr, 16); unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51", pt, 32);
      ami_aes128_ctr(key, ctr, pt, ct, 32); hex(ct, 32, h); CHECK(!strcmp(h, "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"), "AES-128-CTR NIST SP 800-38A F.5.1 (two blocks, counter carry)");
      ami_aes128_ctr(key, ctr, ct, ct, 32); CHECK(!memcmp(ct, pt, 32), "CTR is its own inverse (in place)");
      ami_aes128_ctr(key, ctr, pt, ct, 20); hex(ct, 20, h); CHECK(!strncmp(h, "874d6191b620e3261bef6864990db6ce9806f66b", 40), "CTR with a partial last block"); }
    { char b[64]; CHECK(ami_b64_encode("foobar", 6, b, sizeof b) == 8 && !strcmp(b, "Zm9vYmFy"), "base64 encode"); CHECK(ami_b64_encode("fo", 2, b, sizeof b) == 4 && !strcmp(b, "Zm8="), "base64 padding");
      uint8_t o[16]; CHECK(ami_b64_decode("Zm9vYmE=", 8, o, sizeof o) == 5 && !memcmp(o, "fooba", 5), "base64 decode"); CHECK(ami_b64_decode("Zm9v*", 5, o, sizeof o) == -1, "bad base64 rejected"); CHECK(ami_b64_encode("abc", 3, b, 4) == 0, "too small output refused"); }

    /* ---- virtual amiibo ---- */
    Amiibo a; amiibo_new(&a, ID_A, 42);
    CHECK(a.uid[0] == 0x04 && !memcmp(a.id, ID_A, 8), "new: NXP UID prefix and the model id");
    Amiibo a2; amiibo_new(&a2, ID_A, 42); CHECK(!memcmp(a.uid, a2.uid, 7), "same seed, same UID"); amiibo_new(&a2, ID_A, 43); CHECK(memcmp(a.uid, a2.uid, 7), "different seed, different UID");
    amiibo_new(&a2, ID_A, 0); Amiibo a3; amiibo_new(&a3, ID_A, 0); CHECK(memcmp(a2.uid, a3.uid, 7) != 0, "seed 0 draws random UIDs");
    CHECK(!a.registered && !a.has_app_area && a.nickname[0] == 0, "fresh amiibo: unregistered, no area");
    uint8_t mii[AMIIBO_MII_CHARINFO]; for (int i = 0; i < AMIIBO_MII_CHARINFO; i++) mii[i] = (uint8_t)(i + 1);
    amiibo_set_register(&a, "Nickname Too Long For Ten", mii, (AmiiboDate){2026, 10, 26});
    CHECK(a.registered && !strcmp(a.nickname, "Nickname T") && !memcmp(a.owner_mii, mii, sizeof mii) && a.register_date.year == 2026, "register: nickname cut at 10 characters");
    amiibo_set_register(&a, "h\xc3\xa9llo w\xc3\xb6rld!!", mii, (AmiiboDate){2026, 10, 26}); CHECK(!strcmp(a.nickname, "h\xc3\xa9llo w\xc3\xb6rl"), "multi-byte characters count as one");
    uint8_t data[40]; for (int i = 0; i < 40; i++) data[i] = (uint8_t)(0xa0 + i);
    CHECK(amiibo_area_create(&a, 0x34f80200, 0x01006f8002326000ull, data, 40, 0) == 0 && a.has_app_area && !memcmp(a.app_data, data, 40) && a.app_data[40] == 0 && a.app_data[215] == 0, "area create pads with the fill byte");
    CHECK(amiibo_area_create(&a, 1, 2, data, 1, 0) == -1, "second create refused"); amiibo_area_delete(&a); CHECK(!a.has_app_area, "delete"); CHECK(amiibo_area_create(&a, 1, 2, data, 217, 0) == -2, "oversize area refused");
    amiibo_area_create(&a, 0x34f80200, 0x01006f8002326000ull, data, 40, 0xcc); CHECK(a.app_data[100] == 0xcc, "custom fill");
    a.write_counter = 7; a.version = 3; a.write_date = (AmiiboDate){2026, 10, 25}; a.font_region = 2;

    /* ---- JSON ---- */
    char js[2048]; size_t jn = amiibo_to_json(&a, js, sizeof js); CHECK(jn > 100 && jn < 1500, "json fits"); CHECK(strstr(js, "\"id\": \"0101000000040002\"") != NULL && strstr(js, "\"access_id\": \"34f80200\"") != NULL, "json carries the id and the access id in hex");
    Amiibo b; CHECK(amiibo_from_json(js, jn, &b) == 0, "json parses");
    CHECK(!memcmp(b.id, a.id, 8) && !memcmp(b.uid, a.uid, 7) && !strcmp(b.nickname, a.nickname) && !memcmp(b.owner_mii, a.owner_mii, sizeof mii) && b.registered && b.register_date.month == 10 && b.write_date.day == 25 && b.write_counter == 7 && b.version == 3 && b.font_region == 2, "json round trip: identity, owner, dates, counters");
    CHECK(b.has_app_area && b.app_access_id == 0x34f80200 && b.app_id == 0x01006f8002326000ull && !memcmp(b.app_data, a.app_data, 216) && b.app_write_counter == a.app_write_counter, "json round trip: the application area");
    { char tiny[64]; CHECK(amiibo_to_json(&a, tiny, sizeof tiny) == 0, "json output too small is refused"); }
    { Amiibo q; amiibo_new(&q, ID_B, 5); amiibo_set_register(&q, "q\"uo\\te\n\x01", NULL, (AmiiboDate){2025, 1, 2}); char j2[2048]; size_t n2 = amiibo_to_json(&q, j2, sizeof j2); Amiibo r; CHECK(amiibo_from_json(j2, n2, &r) == 0 && !strcmp(r.nickname, q.nickname), "quotes, backslashes and control characters survive"); }
    CHECK(amiibo_from_json("{\"id\":\"0101000000040002\"}", 25, &b) == 0 && !b.registered && !b.has_app_area && b.uid[0] == 0x04, "a hand-written file needs only the id (a UID is generated)");
    CHECK(amiibo_from_json("{\"id\":\"01010000\"}", 17, &b) == -2 && amiibo_from_json("{\"id\":12}", 9, &b) == -2 && amiibo_from_json("{\"nickname\":\"x\"}", 16, &b) == -2, "missing or bad id");
    CHECK(amiibo_from_json("{\"id\": \"0101000000040002\", \"uid\": \"zz\"}", 38, &b) == -1 && amiibo_from_json("not json", 8, &b) == -1 && amiibo_from_json("{\"id\":\"0101000000040002\"", 24, &b) == 0, "bad uid and garbage rejected (an unterminated object with a valid id is read leniently)");
    { const char *doc = "{\"id\":\"0101000000040002\",\"app_area\":{\"access_id\":\"zz\"}}"; CHECK(amiibo_from_json(doc, strlen(doc), &b) == -1, "bad access id rejected"); }
    { const char *doc = "{\"id\":\"0101000000040002\",\"future_field\":{\"nested\":[1,2,{\"x\":\"}\"}]},\"nickname\":\"ok\"}"; CHECK(amiibo_from_json(doc, strlen(doc), &b) == 0 && !strcmp(b.nickname, "ok"), "unknown fields (even nested, with braces inside strings) are skipped"); }

    /* ---- store ---- */
    CHECK(amiibo_store_save(store, &a) == 0, "store save creates the directory");
    { char path[300], uh[15]; amiibo_uid_to_hex(a.uid, uh); snprintf(path, sizeof path, "%s/%s.json", store, uh); CHECK(access(path, R_OK) == 0, "file named after the UID"); snprintf(path, sizeof path, "%s/%s.json.tmp", store, uh); CHECK(access(path, F_OK) != 0, "no temporary file left behind"); }
    Amiibo l; CHECK(amiibo_store_load(store, a.uid, &l) == 0 && !strcmp(l.nickname, a.nickname) && l.has_app_area, "store load"); { uint8_t nouid[7] = {1, 2, 3, 4, 5, 6, 7}; CHECK(amiibo_store_load(store, nouid, &l) == -1, "missing file: -1"); }
    Amiibo c1; amiibo_new(&c1, ID_B, 100); amiibo_store_save(store, &c1); Amiibo c2; amiibo_new(&c2, ID_A, 101); amiibo_store_save(store, &c2);
    uint8_t uids[8][7]; CHECK(amiibo_store_list(store, uids, 8) == 3, "three stored amiibo"); CHECK(memcmp(uids[0], uids[1], 7) < 0 && memcmp(uids[1], uids[2], 7) < 0, "list is sorted by UID");
    CHECK(amiibo_store_find_by_id(store, ID_B, &l) == 0 && !memcmp(l.uid, c1.uid, 7), "find by model id"); { uint8_t other[8] = {9, 9, 9, 9, 9, 9, 9, 9}; CHECK(amiibo_store_find_by_id(store, other, &l) == -1, "no match: -1"); }
    { char path[300], uh[15]; amiibo_uid_to_hex(c1.uid, uh); snprintf(path, sizeof path, "%s/%s.json", store, uh); FILE *f = fopen(path, "w"); fputs("{broken", f); fclose(f); CHECK(amiibo_store_load(store, c1.uid, &l) == -2, "a damaged file is reported, not trusted"); }
    CHECK(amiibo_store_delete(store, c1.uid) == 0 && amiibo_store_list(store, uids, 8) == 2 && amiibo_store_delete(store, c1.uid) == -1, "delete");
    CHECK(amiibo_store_save("/proc/napple-nope/x", &a) == -1, "an unwritable store fails cleanly");

    /* ---- plain layout and dump crypto (test-only key) ---- */
    AmiiboKeys keys, wrong; test_keys(&keys, 1); test_keys(&wrong, 2);
    uint8_t plain[AMIIBO_TAG_SIZE], tag[AMIIBO_TAG_SIZE], back[AMIIBO_TAG_SIZE];
    amiibo_to_plain(&a, plain); CHECK(plain[0x28] == 0xa5 && plain[0x29] == 0 && plain[0x2a] == 7 && !memcmp(plain + 0x1dc, ID_A, 8) && plain[0x1d4] == a.uid[0], "plain layout: constant, write counter, model id, UID");
    CHECK(plain[0x1d7] == (0x88 ^ a.uid[0] ^ a.uid[1] ^ a.uid[2]) && plain[0] == (a.uid[3] ^ a.uid[4] ^ a.uid[5] ^ a.uid[6]), "UID check bytes");
    Amiibo pa; CHECK(amiibo_from_plain(plain, &pa) == 0 && !memcmp(pa.uid, a.uid, 7) && !strcmp(pa.nickname, a.nickname) && pa.has_app_area && pa.app_access_id == a.app_access_id && !memcmp(pa.app_data, a.app_data, 216) && pa.write_date.year == 2026 && pa.register_date.day == 26 && pa.registered, "plain round trip");
    plain[0x28] = 0; CHECK(amiibo_from_plain(plain, &pa) == -2, "not an amiibo layout"); amiibo_to_plain(&a, plain);
    amiibo_tag_pack(&keys, plain, tag);
    CHECK(tag[0] == a.uid[0] && tag[3] == (0x88 ^ a.uid[0] ^ a.uid[1] ^ a.uid[2]) && !memcmp(tag + 0x54, ID_A, 8), "tag image: UID and model id in the clear at their hardware pages");
    CHECK(memcmp(tag + 0xa0, plain + 0x4c, 0x60) != 0, "the owner / area region is encrypted");
    CHECK(amiibo_tag_unpack(&keys, tag, back) == 0 && !memcmp(back + 0x2c, plain + 0x2c, 0x188) && !memcmp(back + 0x1d4, plain + 0x1d4, 0x34), "unpack with the right key restores the plain data");
    CHECK(amiibo_tag_unpack(&wrong, tag, back) == -1, "a different key fails verification");
    { uint8_t t2[AMIIBO_TAG_SIZE]; memcpy(t2, tag, sizeof t2); t2[0xb0] ^= 1; CHECK(amiibo_tag_unpack(&keys, t2, back) == -1, "a flipped bit in the encrypted region fails verification");
      memcpy(t2, tag, sizeof t2); t2[1] ^= 1; CHECK(amiibo_tag_unpack(&keys, t2, back) == -1, "a changed UID fails verification");
      memcpy(t2, tag, sizeof t2); t2[0x90] ^= 1; CHECK(amiibo_tag_unpack(&keys, t2, back) == -1, "a changed signature fails verification"); }
    { uint8_t t2[AMIIBO_TAG_SIZE]; amiibo_tag_pack(&keys, plain, t2); CHECK(!memcmp(t2, tag, sizeof t2), "packing is deterministic"); plain[0x2a] = 8; amiibo_tag_pack(&keys, plain, t2); CHECK(memcmp(t2, tag, 0x200) != 0, "a different write counter changes the keystream"); plain[0x2a] = 7; }
    Amiibo imp; CHECK(amiibo_import_dump(&keys, tag, &imp) == 0 && !memcmp(imp.id, a.id, 8) && !memcmp(imp.uid, a.uid, 7) && !strcmp(imp.nickname, a.nickname) && imp.has_app_area, "import dump");
    CHECK(imp.has_owner_raw, "the 3DS-format owner bytes are kept"); { uint8_t t3[AMIIBO_TAG_SIZE]; amiibo_export_dump(&keys, &imp, t3); Amiibo again; CHECK(amiibo_import_dump(&keys, t3, &again) == 0 && !strcmp(again.nickname, imp.nickname) && !memcmp(again.app_data, imp.app_data, 216), "export then import"); }
    CHECK(amiibo_import_dump(&wrong, tag, &imp) == -1, "import with the wrong key");
    { char kf[300]; snprintf(kf, sizeof kf, "%s/key_retail.bin", root); AmiiboKeys k2; CHECK(amiibo_keys_load(kf, &k2) == -1, "missing key file: -1");
      FILE *f = fopen(kf, "wb"); fwrite(keys.data, 1, 80, f); fclose(f); CHECK(amiibo_keys_load(kf, &k2) == -2, "short key file: -2");
      f = fopen(kf, "wb"); fwrite(keys.data, 1, 80, f); fwrite(keys.tag, 1, 80, f); fclose(f); CHECK(amiibo_keys_load(kf, &k2) == 0 && !memcmp(&k2, &keys, sizeof k2), "160-byte key file: data keys first, then tag keys"); }

    /* ---- library ---- */
    AmiiboLib lib; amiibo_lib_init(&lib);
    CHECK(amiibo_lib_load_text(&lib, API_DOC, sizeof API_DOC - 1) == 2, "AmiiboAPI document: two valid entries, the broken one skipped");
    CHECK(!strcmp(lib.items[0].name, "alpha card") && !strcmp(lib.items[1].name, "Zed Figure"), "sorted by name, case-insensitively");
    CHECK(!strcmp(lib.items[1].id, "0101000000040002") && !strcmp(lib.items[1].series, "Test Series") && !strcmp(lib.items[1].character, "Zed") && !strcmp(lib.items[1].game_series, "Test Game") && !strcmp(lib.items[1].type, "Figure") && !strcmp(lib.items[1].image, "https://example.invalid/i.png"), "entry fields");
    CHECK(amiibo_lib_find(&lib, ID_A) == &lib.items[1] && amiibo_lib_find(&lib, ID_B) == &lib.items[0], "find by model id");
    size_t idx[8]; CHECK(amiibo_lib_search(&lib, "zed", idx, 8) == 1 && idx[0] == 1, "search is case-insensitive"); CHECK(amiibo_lib_search(&lib, "test game", idx, 8) == 2, "search matches the game series"); CHECK(amiibo_lib_search(&lib, "", idx, 1) == 2 && idx[0] == 0, "an empty query lists everything (count is the total, idx honours max)");
    CHECK(amiibo_lib_search(&lib, "0004000", idx, 8) == 1 && amiibo_lib_search(&lib, "nothing", idx, 8) == 0, "search by id fragment, and no match");
    CHECK(amiibo_lib_load_text(&lib, "{\"amiibo\":[]}", 13) == -1 && lib.count == 2, "an empty document is rejected and the old contents stay");
    CHECK(amiibo_lib_load_text(&lib, "garbage", 7) == -1 && amiibo_lib_load_text(&lib, "{\"amiibo\":[{", 12) == -1 && lib.count == 2, "malformed documents are rejected");
    CHECK(amiibo_lib_load_text(&lib, DB_DOC, sizeof DB_DOC - 1) == 2 && !strcmp(lib.items[0].name, "Db One") && !strcmp(lib.items[1].id, "01010000000400aa"), "repository database format (ids lower-cased)");
    amiibo_lib_free(&lib);
    { AmiiboConfig cfg; setenv("NAPPLE_AMIIBO_DB", "https://mirror.invalid/amiibo/", 1); amiibo_config_default(&cfg, root);
      CHECK(amiibo_config_source_is_remote(&cfg) && !strcmp(cfg.source, "https://mirror.invalid/amiibo/"), "NAPPLE_AMIIBO_DB overrides the source"); unsetenv("NAPPLE_AMIIBO_DB"); amiibo_config_default(&cfg, root);
      CHECK(!strcmp(cfg.source, AMIIBO_DEFAULT_SOURCE), "the default source is the documented configuration value"); CHECK(strstr(cfg.store, "/system/amiibo") && strstr(cfg.cache, "/system/amiibo-cache/amiibo.json") && strstr(cfg.key, "key_retail.bin"), "default paths");
      snprintf(cfg.cache, sizeof cfg.cache, "%s", cache); snprintf(cfg.source, sizeof cfg.source, "%s/mirror", root); amiibo_lib_init(&lib);
      CHECK(amiibo_lib_refresh(&lib, &cfg, NULL, 0) == -1 && lib.count == 0, "nothing fetched, no cache, no local source: -1, empty library");
      CHECK(amiibo_lib_refresh(&lib, &cfg, API_DOC, sizeof API_DOC - 1) == 1 && lib.count == 2 && access(cache, R_OK) == 0, "a fetched document is used and cached");
      amiibo_lib_free(&lib); amiibo_lib_init(&lib); CHECK(amiibo_lib_refresh(&lib, &cfg, NULL, 0) == 2 && lib.count == 2, "offline: the cache is the fallback");
      CHECK(amiibo_lib_refresh(&lib, &cfg, "not json", 8) == 2 && lib.count == 2, "a bad fetch is ignored and the cache stays");
      unlink(cache); char mdir[300], mf[340]; snprintf(mdir, sizeof mdir, "%s/mirror", root); mkdir(mdir, 0755); snprintf(mdir, sizeof mdir, "%s/mirror/database", root); mkdir(mdir, 0755); snprintf(mf, sizeof mf, "%s/amiibo.json", mdir); FILE *f = fopen(mf, "w"); fputs(DB_DOC, f); fclose(f);
      amiibo_lib_free(&lib); amiibo_lib_init(&lib); CHECK(amiibo_lib_refresh(&lib, &cfg, NULL, 0) == 3 && lib.count == 2, "a local mirror folder is the last resort (database/amiibo.json layout)");
      snprintf(cfg.source, sizeof cfg.source, "https://x.invalid/"); amiibo_lib_free(&lib); amiibo_lib_init(&lib); CHECK(amiibo_lib_refresh(&lib, &cfg, NULL, 0) == -1, "a remote source is never read as a path");
      amiibo_lib_free(&lib); }

    /* ---- picker ---- */
    AmiiboConfig cfg; amiibo_config_default(&cfg, root); snprintf(cfg.store, sizeof cfg.store, "%s/pstore", root); snprintf(cfg.key, sizeof cfg.key, "%s/nokey/key_retail.bin", root);
    AmiiboPicker pk; amiibo_picker_init(&pk, &cfg); const NfcTagSource *src = amiibo_picker_source(&pk);
    NfcTag t; uint64_t g0 = 0, g1 = 0;
    CHECK(src->poll(src->ctx, &t, &g0) == 0, "nothing on the reader at first");
    fired = 0; src->set_listener(src->ctx, on_change, NULL);
    CHECK(amiibo_picker_select(&pk, ID_A, AMIIBO_PICK_REUSE) == 0 && fired == 1 && amiibo_picker_present(&pk), "select places an amiibo and notifies the listener");
    CHECK(src->poll(src->ctx, &t, &g1) == 1 && g1 != g0 && t.is_amiibo && !memcmp(t.amiibo.id, ID_A, 8) && !memcmp(t.uid, t.amiibo.uid, 7), "poll returns the tag, the generation moved");
    uint8_t first_uid[7]; memcpy(first_uid, t.uid, 7);
    { uint8_t u[8][7]; CHECK(amiibo_store_list(cfg.store, u, 8) == 1, "the virtual amiibo was stored at once"); }
    amiibo_picker_remove(&pk); CHECK(!amiibo_picker_present(&pk) && fired == 2 && src->poll(src->ctx, &t, &g1) == 0, "remove empties the reader and notifies"); amiibo_picker_remove(&pk); CHECK(fired == 2, "removing nothing does not notify");
    amiibo_picker_select(&pk, ID_A, AMIIBO_PICK_REUSE); src->poll(src->ctx, &t, &g1); CHECK(!memcmp(t.uid, first_uid, 7), "reuse: the same stored amiibo comes back (its progress persists)");
    amiibo_picker_select(&pk, ID_A, AMIIBO_PICK_NEW); src->poll(src->ctx, &t, &g1); CHECK(memcmp(t.uid, first_uid, 7) != 0, "new: another amiibo of the same model with a new UID");
    { uint8_t u[8][7]; CHECK(amiibo_store_list(cfg.store, u, 8) == 2, "now two are stored"); }
    CHECK(amiibo_picker_select_hex(&pk, "0x0000000000000009", AMIIBO_PICK_REUSE) == 0, "hex id with 0x prefix"); CHECK(amiibo_picker_select_hex(&pk, "123", AMIIBO_PICK_REUSE) == -1 && strstr(pk.last_error, "123"), "bad id: -1 with a message");
    CHECK(amiibo_picker_select_uid(&pk, first_uid) == 0, "select by stored UID"); { uint8_t none[7] = {0}; CHECK(amiibo_picker_select_uid(&pk, none) == -1, "unknown UID: -1"); }
    { NfcTag w; src->poll(src->ctx, &w, &g1); w.amiibo.write_counter = 9; CHECK(src->store(src->ctx, &w) == 0, "store"); Amiibo ld; CHECK(amiibo_store_load(cfg.store, w.uid, &ld) == 0 && ld.write_counter == 9, "the write reached the store"); src->poll(src->ctx, &t, &g1); CHECK(t.amiibo.write_counter == 9, "and the reader shows it"); }
    { NfcTag raw; memset(&raw, 0, sizeof raw); CHECK(src->store(src->ctx, &raw) != 0, "a tag that is not an amiibo cannot be stored"); }
    size_t n = amiibo_lib_load_text(&pk.lib, API_DOC, sizeof API_DOC - 1); CHECK(n == 2 && amiibo_picker_count(&pk) == 2 && !strcmp(amiibo_picker_get(&pk, 1)->name, "Zed Figure") && amiibo_picker_get(&pk, 5) == NULL, "picker list"); CHECK(amiibo_picker_search(&pk, "card", idx, 8) == 1, "picker search");
    /* dump import through the picker */
    char dump[300]; snprintf(dump, sizeof dump, "%s/amiibo.bin", root); { FILE *f = fopen(dump, "wb"); fwrite(tag, 1, AMIIBO_TAG_SIZE, f); fclose(f); }
    CHECK(amiibo_picker_import_dump(&pk, dump) == -2 && strstr(pk.last_error, cfg.key) && strstr(pk.last_error, "key_retail.bin") && strstr(pk.last_error, "missing"), "no key file: -2 and the message names the exact missing file");
    snprintf(pk.cfg.key, sizeof pk.cfg.key, "%s/key_retail.bin", root); CHECK(amiibo_picker_import_dump(&pk, dump) == 0, "with the key file the dump becomes a virtual amiibo"); src->poll(src->ctx, &t, &g1); CHECK(!strcmp(t.amiibo.nickname, a.nickname) && !memcmp(t.uid, a.uid, 7), "and sits on the reader");
    { char kf[300]; snprintf(kf, sizeof kf, "%s/bad_key.bin", root); FILE *f = fopen(kf, "wb"); fwrite(keys.data, 1, 80, f); fclose(f); snprintf(pk.cfg.key, sizeof pk.cfg.key, "%s", kf); CHECK(amiibo_picker_import_dump(&pk, dump) == -3, "invalid key file: -3");
      AmiiboKeys wk; test_keys(&wk, 9); f = fopen(kf, "wb"); fwrite(wk.data, 1, 80, f); fwrite(wk.tag, 1, 80, f); fclose(f); CHECK(amiibo_picker_import_dump(&pk, dump) == -4, "wrong key: -4"); }
    { char small[300]; snprintf(small, sizeof small, "%s/small.bin", root); FILE *f = fopen(small, "wb"); fputs("short", f); fclose(f); CHECK(amiibo_picker_import_dump(&pk, small) == -1 && amiibo_picker_import_dump(&pk, "/nonexistent.bin") == -1, "unreadable or short dump: -1"); }
    /* scripted selector */
    AmiiboScript sc; CHECK(amiibo_script_parse("0101000000040002,250,1000", &sc) == 0 && sc.valid && sc.insert_ms == 250 && sc.remove_ms == 1000 && !strcmp(sc.what, "0101000000040002"), "script: id, insert and remove delays");
    CHECK(amiibo_script_parse("0101000000040002", &sc) == 0 && sc.insert_ms == 0 && sc.remove_ms == 0, "script: bare id"); CHECK(amiibo_script_parse("x,abc", &sc) == -1 && amiibo_script_parse("", &sc) == -1 && amiibo_script_parse(NULL, &sc) == -1, "bad scripts");
    amiibo_script_parse("0101000000040002", &sc); amiibo_picker_remove(&pk); CHECK(amiibo_script_apply(&pk, &sc) == 0 && amiibo_picker_present(&pk), "script by model id");
    { char uh[15]; amiibo_uid_to_hex(first_uid, uh); amiibo_picker_remove(&pk); amiibo_script_parse(uh, &sc); CHECK(amiibo_script_apply(&pk, &sc) == 0 && amiibo_picker_present(&pk), "script by stored UID"); }
    amiibo_script_parse(dump, &sc); snprintf(pk.cfg.key, sizeof pk.cfg.key, "%s/key_retail.bin", root); amiibo_picker_remove(&pk); CHECK(amiibo_script_apply(&pk, &sc) == 0 && amiibo_picker_present(&pk), "script by dump path");
    amiibo_picker_free(&pk);
    char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf %s", root); (void)!system(cmd);
    printf("{\"test\":\"svc_amiibo\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
