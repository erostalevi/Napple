/* Unit tests: bcat name validation, progress record and the offline tables. */
#include "../src/services/bcat.c"
#include "svc_test.h"
#include <sys/stat.h>
#include <unistd.h>

static void name(char out[BCAT_NAME_SIZE], const char *s) { memset(out, 0, BCAT_NAME_SIZE); strncpy(out, s, BCAT_NAME_SIZE); }

int main(void) {
    char n[BCAT_NAME_SIZE];
    name(n, "news_2026-10"); CHECK(bcat_valid_directory_name(n) && bcat_valid_file_name(n), "plain name");
    name(n, "a.b"); CHECK(!bcat_valid_directory_name(n) && bcat_valid_file_name(n), "dot allowed in files only");
    name(n, ""); CHECK(!bcat_valid_directory_name(n), "empty name");
    name(n, "bad/name"); CHECK(!bcat_valid_directory_name(n) && !bcat_valid_file_name(n), "slash rejected");
    memset(n, 'a', BCAT_NAME_SIZE); CHECK(!bcat_valid_directory_name(n), "no room for the NUL");
    memset(n, 'a', 31); n[31] = 0; CHECK(bcat_valid_directory_name(n), "31 characters fit");
    name(n, "ok"); n[10] = 'x'; CHECK(!bcat_valid_directory_name(n), "garbage after the NUL");
    uint8_t rec[BCAT_PROGRESS_SIZE]; bcat_make_progress(BCAT_STATUS_DONE, 0x1234, rec);
    CHECK(rec[0] == BCAT_STATUS_DONE && rec[4] == 0x34 && rec[5] == 0x12 && rec[8] == 0 && rec[BCAT_PROGRESS_SIZE - 1] == 0, "progress record layout");
    { BcatProgress pr = {BCAT_STATUS_DOWNLOADING, 0, "news", "a.bin", 10, 20, 30, 40}; uint8_t full[BCAT_PROGRESS_SIZE]; bcat_make_progress_full(&pr, full); int64_t v;
      CHECK(full[0] == BCAT_STATUS_DOWNLOADING && !memcmp(full + 8, "news", 5) && !memcmp(full + 0x28, "a.bin", 6) && full[8 + 5] == 0 && full[0x27] == 0, "progress: names at +0x08 and +0x28");
      memcpy(&v, full + 0x48, 8); CHECK(v == 10, "progress: current downloaded at +0x48"); memcpy(&v, full + 0x50, 8); CHECK(v == 20, "current total at +0x50");
      memcpy(&v, full + 0x58, 8); CHECK(v == 30, "total downloaded at +0x58"); memcpy(&v, full + 0x60, 8); CHECK(v == 40, "total total at +0x60");
      int z = 1; for (unsigned i = 0x68; i < BCAT_PROGRESS_SIZE; i++) if (full[i]) z = 0; CHECK(z, "progress: reserved tail is zero");
      char long_name[64]; memset(long_name, 'x', 63); long_name[63] = 0; pr.directory = long_name; bcat_make_progress_full(&pr, full); CHECK(full[8 + 31] == 0 && full[8 + 30] == 'x', "progress: long names are truncated, NUL kept"); }
    { BcatFile f; memset(&f, 0, sizeof f); strcpy(f.name, "x.bin"); f.size = 0x1122334455667788ll; for (int i = 0; i < 16; i++) f.digest[i] = (uint8_t)(0xa0 + i);
      uint8_t e[BCAT_ENTRY_SIZE]; bcat_pack_entry(&f, e); uint64_t sz; memcpy(&sz, e + 0x20, 8);
      CHECK(BCAT_ENTRY_SIZE == 0x38 && !memcmp(e, "x.bin", 6) && sz == 0x1122334455667788ull && e[0x28] == 0xa0 && e[0x37] == 0xaf, "directory entry: name 0x20, size at +0x20, md5 at +0x28, 0x38 bytes");
      uint8_t d[16]; bcat_md5("", 0, d); CHECK(d[0] == 0xd4 && d[1] == 0x1d && d[15] == 0x7e, "md5 of the empty string (RFC 1321 vector)");
      bcat_md5("abc", 3, d); CHECK(d[0] == 0x90 && d[1] == 0x01 && d[15] == 0x72, "md5 of abc (RFC 1321 vector)"); }

    IpcObject *root = bcat_open(); IpcCall c;
    svc_call(root, 0, NULL, 0, &c); IpcObject *bs = c.out_objects[0];
    svc_call(bs, 10100, NULL, 0, &c); CHECK(c.nout_objects == 1, "RequestSyncDeliveryCache returns a progress service");
    IpcObject *pg = c.out_objects[0];
    svc_call(pg, 0, NULL, 0, &c); CHECK(c.nout_copy == 1 && event_is_signaled(c.out_copy[0]), "progress event is already signalled (sync finished)");
    uint8_t big[BCAT_PROGRESS_SIZE]; memset(&c, 0, sizeof c); svc_set_out_buf(&c, big, sizeof big); svc_call_c(pg, 1, &c);
    uint32_t st, res; memcpy(&st, big, 4); memcpy(&res, big + 4, 4);
    CHECK(st == BCAT_STATUS_DONE && res == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "progress: done, with the offline result");
    svc_call(root, 1, NULL, 0, &c); IpcObject *stg = c.out_objects[0];
    svc_call(stg, 1, NULL, 0, &c); IpcObject *dir = c.out_objects[0];
    name(n, "news"); svc_call(dir, 0, n, sizeof n, &c); CHECK(c.result == HZ_R_BCAT_NOT_FOUND, "Open of a valid but absent directory: not found");
    name(n, "../x"); svc_call(dir, 0, n, sizeof n, &c); CHECK(c.result != 0 && c.result != HZ_R_BCAT_NOT_FOUND, "invalid directory name is a different error");
    svc_call(dir, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "directory count 0");
    svc_call(stg, 0, NULL, 0, &c); IpcObject *fs = c.out_objects[0];
    char names[2 * BCAT_NAME_SIZE]; name(names, "news"); name(names + BCAT_NAME_SIZE, "list.json");
    svc_call(fs, 0, names, sizeof names, &c); CHECK(c.result == HZ_R_BCAT_NOT_FOUND, "file Open: not found");
    svc_call(stg, 10, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "no cached directories");
    /* SetPassphrase validates the buffer size; the delivery tasks are accepted and the task list stays empty */
    uint8_t pass[0x42] = {0}; IpcCall pc; memset(&pc, 0, sizeof pc); svc_set_in_buf(&pc, pass, 0x41);
    svc_call(root, 0, NULL, 0, &c); IpcObject *bsv = c.out_objects[0];
    svc_call_c(bsv, 30100, &pc); CHECK(pc.result == 0, "passphrase of 0x41 bytes accepted");
    memset(&pc, 0, sizeof pc); svc_set_in_buf(&pc, pass, 0x42); svc_call_c(bsv, 30100, &pc); CHECK(pc.result != 0, "passphrase too long");
    svc_call(bsv, 30200, NULL, 0, &c); CHECK(c.result == 0, "register task"); svc_call(bsv, 90100, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "no tasks");
    svc_call(bsv, 90201, NULL, 0, &c); CHECK(c.result == 0, "clear storage");

    /* MD5 vectors */
    { uint8_t d[16];
      static const uint8_t abc[16] = {0x90,0x01,0x50,0x98,0x3c,0xd2,0x4f,0xb0,0xd6,0x96,0x3f,0x7d,0x28,0xe1,0x7f,0x72};
      static const uint8_t empty[16] = {0xd4,0x1d,0x8c,0xd9,0x8f,0x00,0xb2,0x04,0xe9,0x80,0x09,0x98,0xec,0xf8,0x42,0x7e};
      static const uint8_t hello[16] = {0x5d,0x41,0x40,0x2a,0xbc,0x4b,0x2a,0x76,0xb9,0x71,0x9d,0x91,0x10,0x17,0xc5,0x92};
      bcat_md5("abc", 3, d); CHECK(!memcmp(d, abc, 16), "md5(abc)");
      bcat_md5("", 0, d); CHECK(!memcmp(d, empty, 16), "md5 of nothing");
      bcat_md5("hello", 5, d); CHECK(!memcmp(d, hello, 16), "md5(hello)");
      static const char m56[] = "12345678901234567890123456789012345678901234567890123456789012345678901234567890"; /* 80 bytes: two blocks with the padding spilling */
      static const uint8_t r80[16] = {0x57,0xed,0xf4,0xa2,0x2b,0xe3,0xc9,0x55,0xac,0x49,0xda,0x2e,0x21,0x07,0xb6,0x7a};
      bcat_md5(m56, 80, d); CHECK(!memcmp(d, r80, 16), "md5 of the RFC 1321 80-digit vector"); }

    /* a provider folder: <root>/news/{list.json,b.bin}, <root>/other/x.txt, a stray file and a bad directory name at the top */
    char rootdir[] = "/tmp/napple-bcat-XXXXXX"; if (!mkdtemp(rootdir)) return 1;
    { char p[600]; const char *dirs[] = {"news", "other", "bad.dir"};
      for (int i = 0; i < 3; i++) { snprintf(p, sizeof p, "%s/%s", rootdir, dirs[i]); mkdir(p, 0755); }
      const struct { const char *path, *data; } files[] = {{"news/list.json", "hello"}, {"news/b.bin", "0123456789"}, {"other/x.txt", ""}, {"stray.txt", "x"}};
      for (unsigned i = 0; i < 4; i++) { snprintf(p, sizeof p, "%s/%s", rootdir, files[i].path); FILE *f = fopen(p, "wb"); fputs(files[i].data, f); fclose(f); } }
    BcatProvider prov = bcat_dir_provider(rootdir); bcat_set_provider(&prov);

    /* sync now succeeds: the data is local */
    svc_call(bs, 10100, NULL, 0, &c); IpcObject *pg2 = c.out_objects[0];
    memset(&c, 0, sizeof c); svc_set_out_buf(&c, big, sizeof big); svc_call_c(pg2, 1, &c); memcpy(&st, big, 4); memcpy(&res, big + 4, 4);
    CHECK(st == BCAT_STATUS_DONE && res == 0, "with a provider the sync result is success");

    /* storage: directories (valid sub-folders only) */
    svc_call(root, 1, NULL, 0, &c); IpcObject *stg2 = c.out_objects[0];
    char names2[8][BCAT_NAME_SIZE]; memset(&c, 0, sizeof c); svc_set_out_buf(&c, names2, sizeof names2); svc_call_c(stg2, 10, &c);
    int nd = SVC_OUT(&c, int32_t, 0); int has_news = 0, has_other = 0, has_bad = 0;
    for (int i = 0; i < nd; i++) { has_news |= !strncmp(names2[i], "news", BCAT_NAME_SIZE); has_other |= !strncmp(names2[i], "other", BCAT_NAME_SIZE); has_bad |= !strncmp(names2[i], "bad.dir", BCAT_NAME_SIZE); }
    CHECK(nd == 2 && has_news && has_other && !has_bad, "EnumerateDeliveryCacheDirectory lists the two valid sub-folders (%d)", nd);
    memset(&c, 0, sizeof c); svc_set_out_buf(&c, names2, BCAT_NAME_SIZE); svc_call_c(stg2, 10, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 1, "a one-name buffer takes one directory");

    /* directory service */
    svc_call(stg2, 1, NULL, 0, &c); IpcObject *dir2 = c.out_objects[0];
    name(n, "news"); svc_call(dir2, 0, n, sizeof n, &c); CHECK(c.result == 0, "Open news");
    svc_call(dir2, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 2, "news holds two files");
    uint8_t ents[4 * BCAT_ENTRY_SIZE]; memset(&c, 0, sizeof c); svc_set_out_buf(&c, ents, BCAT_ENTRY_SIZE); svc_call_c(dir2, 1, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == 1 && !strcmp((char *)ents, "b.bin"), "Read returns files sorted by name, one fits the buffer");
    { uint64_t size; memcpy(&size, ents + 0x20, 8); CHECK(size == 10, "entry size"); }
    memset(&c, 0, sizeof c); svc_set_out_buf(&c, ents, sizeof ents); svc_call_c(dir2, 1, &c);
    CHECK(SVC_OUT(&c, int32_t, 0) == 1 && !strcmp((char *)ents, "list.json"), "the cursor continues");
    { uint8_t want[16]; bcat_md5("hello", 5, want); CHECK(!memcmp(ents + 0x28, want, 16), "entry digest is the MD5 of the content"); }
    memset(&c, 0, sizeof c); svc_set_out_buf(&c, ents, sizeof ents); svc_call_c(dir2, 1, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "end of directory");
    name(n, "missing"); svc_call(dir2, 0, n, sizeof n, &c); CHECK(c.result == HZ_R_BCAT_NOT_FOUND, "absent directory: not found");
    svc_call(dir2, 3, NULL, 0, &c); svc_call(dir2, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "closed directory reports nothing");

    /* file service */
    svc_call(stg2, 0, NULL, 0, &c); IpcObject *fs2 = c.out_objects[0];
    name(names, "news"); name(names + BCAT_NAME_SIZE, "b.bin");
    svc_call(fs2, 0, names, sizeof names, &c); CHECK(c.result == 0, "file Open");
    svc_call(fs2, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, int64_t, 0) == 10, "GetSize");
    { uint8_t out[16]; int64_t off = 3; memset(&c, 0, sizeof c); svc_set_out_buf(&c, out, 4); c.in = (const uint8_t *)&off; c.in_size = 8; svc_call_c(fs2, 1, &c);
      CHECK(c.result == 0 && SVC_OUT(&c, int64_t, 0) == 4 && !memcmp(out, "3456", 4), "Read at offset 3, 4 bytes");
      off = 8; memset(&c, 0, sizeof c); svc_set_out_buf(&c, out, sizeof out); c.in = (const uint8_t *)&off; c.in_size = 8; svc_call_c(fs2, 1, &c);
      CHECK(SVC_OUT(&c, int64_t, 0) == 2 && !memcmp(out, "89", 2), "Read stops at the end of the file");
      off = 10; memset(&c, 0, sizeof c); svc_set_out_buf(&c, out, sizeof out); c.in = (const uint8_t *)&off; c.in_size = 8; svc_call_c(fs2, 1, &c);
      CHECK(c.result == 0 && SVC_OUT(&c, int64_t, 0) == 0, "Read at the end gives 0 bytes");
      off = -1; memset(&c, 0, sizeof c); svc_set_out_buf(&c, out, sizeof out); c.in = (const uint8_t *)&off; c.in_size = 8; svc_call_c(fs2, 1, &c); CHECK(c.result != 0, "negative offset rejected"); }
    { uint8_t want[16]; bcat_md5("0123456789", 10, want); svc_call(fs2, 3, NULL, 0, &c); CHECK(c.out_size == 16 && !memcmp(c.out, want, 16), "GetDigest"); }
    name(names + BCAT_NAME_SIZE, "nope.bin"); svc_call(fs2, 0, names, sizeof names, &c); CHECK(c.result == HZ_R_BCAT_NOT_FOUND, "absent file: not found");
    name(names + BCAT_NAME_SIZE, "../x"); svc_call(fs2, 0, names, sizeof names, &c); CHECK(c.result != 0 && c.result != HZ_R_BCAT_NOT_FOUND, "path traversal is an invalid name, never opened");
    name(names, "other"); name(names + BCAT_NAME_SIZE, "x.txt"); svc_call(fs2, 0, names, sizeof names, &c); svc_call(fs2, 2, NULL, 0, &c);
    CHECK(SVC_OUT(&c, int64_t, 0) == 0, "an empty file opens with size 0");
    svc_call(bsv, 90201, NULL, 0, &c); CHECK(c.result == 0, "ClearDeliveryCacheStorage succeeds");
    { char p[600]; snprintf(p, sizeof p, "%s/news/b.bin", rootdir); CHECK(access(p, R_OK) == 0, "...and never deletes the provider's folder"); }

    /* removing the provider restores the offline answers */
    bcat_set_provider(NULL);
    name(n, "news"); svc_call(dir2, 0, n, sizeof n, &c); CHECK(c.result == HZ_R_BCAT_NOT_FOUND, "no provider: not found again");
    svc_call(bs, 10100, NULL, 0, &c); IpcObject *pg3 = c.out_objects[0];
    memset(&c, 0, sizeof c); svc_set_out_buf(&c, big, sizeof big); svc_call_c(pg3, 1, &c); memcpy(&res, big + 4, 4);
    CHECK(res == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "no provider: the sync fails offline");
    printf("{\"test\":\"svc_bcat\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
