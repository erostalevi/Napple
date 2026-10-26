/* Unit tests: bcat name validation, progress record and the offline tables. */
#include "../src/services/bcat.c"
#include "svc_test.h"

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
    printf("{\"test\":\"svc_bcat\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
