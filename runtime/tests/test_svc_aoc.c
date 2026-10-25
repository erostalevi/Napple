/* Unit tests: aoc:u (add-on content) against manifests like the ones `napple build` writes, and the aoc_core set. misc.c is #included; on Linux the
 * JSON reader (src/json.c, portable) is included too since the Linux check build links only the *_core.c files. */
#include "../src/services/misc.c"
#ifdef __linux__
#include "../src/json.c"
#endif
#include "svc_test.h"
#include <unistd.h>

#define APP 0x0100000000010000ULL   /* add-on base id 0x0100000000011000 */
static const char *MANIFEST =
    "{\"format\":1,\"title_id\":\"0100000000010000\",\"dlc\":["
    "{\"title_id\":\"0100000000011002\",\"version\":65536,\"dir\":\"../dlc/0100000000011002/v65536\",\"romfs\":\"romfs.bin\",\"default_enabled\":true},"
    "{\"title_id\":\"0100000000011001\",\"version\":0,\"dir\":\"../dlc/0100000000011001/v0\",\"romfs\":\"romfs.bin\",\"default_enabled\":true},"
    "{\"title_id\":\"0100000000011003\",\"version\":0,\"dir\":\"../dlc/0100000000011003/v0\",\"romfs\":\"romfs.bin\",\"default_enabled\":false},"
    "{\"title_id\":\"0100000000019001\",\"version\":0,\"dir\":\"../dlc/x\",\"romfs\":\"romfs.bin\"},"
    "{\"title_id\":\"0100000000011001\",\"version\":0,\"dir\":\"../dlc/dup\",\"romfs\":\"romfs.bin\"},"
    "{\"version\":1}]}";

static void reset(const char *text) {
    g_aoc_loaded = 1; aoc_load_manifest(&g_aoc, g_program_id, "/data/game/v1", text, text ? strlen(text) : 0);
}

int main(void) {
    g_program_id = APP;
    CHECK(aoc_base_id(APP) == 0x0100000000011000ULL, "base id");
    AocSet s; aoc_init(&s, APP);
    CHECK(aoc_add(&s, 0x0100000000011003ULL, 0, "c") && aoc_add(&s, 0x0100000000011001ULL, 0, "a") && aoc_add(&s, 0x0100000000011002ULL, 0, "b"), "adds");
    CHECK(!aoc_add(&s, 0x0100000000011000ULL, 0, "x") && !aoc_add(&s, 0x0100000000012000ULL, 0, "x") && !aoc_add(&s, 0x0100000000011001ULL, 0, "x") && !aoc_add(&s, 0x0100000000011004ULL, 0, NULL), "rejects base id itself, out of range, duplicate, no path");
    int32_t idx[8]; CHECK(aoc_count(&s) == 3 && aoc_list(&s, 0, 8, idx) == 3 && idx[0] == 1 && idx[1] == 2 && idx[2] == 3, "sorted by index");
    CHECK(aoc_list(&s, 1, 1, idx) == 1 && idx[0] == 2 && aoc_list(&s, 3, 8, idx) == 0 && aoc_list(&s, 9, 8, idx) == 0 && aoc_list(&s, 0, 0, idx) == 0, "list offset/count/past end");
    CHECK(aoc_find_index(&s, 2) && !strcmp(aoc_find_index(&s, 2)->romfs, "b") && !aoc_find_index(&s, 4) && !aoc_find_index(&s, 0) && !aoc_find_index(&s, -1), "find by index");
    CHECK(aoc_find_data_id(&s, 0x0100000000011003ULL) && !aoc_find_data_id(&s, 0x0100000000011004ULL), "find by data id");

    /* no DLC: today's answers */
    reset(NULL);
    IpcObject *o = aoc_open(); IpcCall c;
    svc_call(o, 2, NULL, 0, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 0, "no manifest: count 0");
    reset("{\"title_id\":\"0100000000010000\"}");
    svc_call(o, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "manifest without dlc: count 0");
    reset("not json"); svc_call(o, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "invalid manifest: count 0");
    int32_t pi = 1; svc_call(o, 7, &pi, 4, &c); CHECK(c.result == R_AOC_NOT_FOUND, "Prepare with no DLC fails");
    CHECK(aoc_data_id_romfs(0x0100000000011001ULL) == NULL, "no DLC: no data id resolves");
    svc_call(o, 5, NULL, 0, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == 0x0100000000011000ULL, "base id unchanged");

    /* with DLC */
    reset(MANIFEST);
    CHECK(g_aoc.n == 2, "disabled, out-of-range, duplicate and id-less entries are dropped (kept %u)", g_aoc.n);
    svc_call(o, 2, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 2, "CountAddOnContent");
    uint64_t app = APP; svc_call(o, 0, &app, 8, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 2, "Count by application id");
    app = APP + 0x10; svc_call(o, 0, &app, 8, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0, "Count for another application: 0");

    int32_t list[4] = {-1, -1, -1, -1}; uint8_t in[16] = {0};
    uint32_t off = 0, cnt = 4; memcpy(in, &off, 4); memcpy(in + 4, &cnt, 4);
    memset(&c, 0, sizeof c); svc_set_out_buf(&c, list, sizeof list); c.in = in; c.in_size = 8; svc_call_c(o, 3, &c);
    CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 2 && list[0] == 1 && list[1] == 2 && list[2] == -1, "ListAddOnContent writes indices 1,2 and returns 2");
    off = 1; cnt = 4; memcpy(in, &off, 4); memcpy(in + 4, &cnt, 4); list[0] = list[1] = -1;
    memset(&c, 0, sizeof c); svc_set_out_buf(&c, list, sizeof list); c.in = in; c.in_size = 8; svc_call_c(o, 3, &c);
    CHECK(SVC_OUT(&c, uint32_t, 0) == 1 && list[0] == 2 && list[1] == -1, "ListAddOnContent offset 1");
    off = 0; cnt = 4; memcpy(in, &off, 4); memcpy(in + 4, &cnt, 4); list[0] = list[1] = -1;
    memset(&c, 0, sizeof c); svc_set_out_buf(&c, list, 4); c.in = in; c.in_size = 8; svc_call_c(o, 3, &c);
    CHECK(SVC_OUT(&c, uint32_t, 0) == 1 && list[0] == 1 && list[1] == -1, "ListAddOnContent is limited by the output buffer size");
    memcpy(in, &off, 4); memcpy(in + 4, &cnt, 4); app = APP; memcpy(in + 8, &app, 8); list[0] = -1;
    memset(&c, 0, sizeof c); svc_set_out_buf(&c, list, sizeof list); c.in = in; c.in_size = 16; svc_call_c(o, 1, &c);
    CHECK(SVC_OUT(&c, uint32_t, 0) == 2 && list[0] == 1, "ListAddOnContentByApplicationId");
    app = 0x42; memcpy(in + 8, &app, 8); list[0] = -1; memset(&c, 0, sizeof c); svc_set_out_buf(&c, list, sizeof list); c.in = in; c.in_size = 16; svc_call_c(o, 1, &c);
    CHECK(SVC_OUT(&c, uint32_t, 0) == 0 && list[0] == -1, "List for another application is empty");

    pi = 2; svc_call(o, 7, &pi, 4, &c); CHECK(c.result == 0, "Prepare(2) succeeds");
    pi = 3; svc_call(o, 7, &pi, 4, &c); CHECK(c.result == R_AOC_NOT_FOUND, "Prepare(3) (disabled DLC) fails");
    pi = 0; svc_call(o, 7, &pi, 4, &c); CHECK(c.result == R_AOC_NOT_FOUND, "Prepare(0) fails");
    { uint64_t pa[2] = {2, APP}; svc_call(o, 6, pa, 16, &c); CHECK(c.result == 0, "PrepareAddOnContentByApplicationId(2, app) succeeds");
      pa[1] = 0x42; svc_call(o, 6, pa, 16, &c); CHECK(c.result == R_AOC_NOT_FOUND, "PrepareAddOnContentByApplicationId for another application fails");
      svc_call(o, 11, pa, 16, &c); CHECK(c.result == 0, "NotifyMountAddOnContent (cmd 11) is accepted"); }
    const char *p = aoc_data_id_romfs(0x0100000000011002ULL);
    CHECK(p && !strcmp(p, "/data/game/v1/../dlc/0100000000011002/v65536/romfs.bin"), "data id resolves to the DLC romfs relative to the data dir (%s)", p ? p : "(null)");
    CHECK(aoc_data_id_romfs(0x0100000000011003ULL) == NULL && aoc_data_id_romfs(APP) == NULL, "unknown data ids do not resolve");
    app = 0x0100000000020000ULL; svc_call(o, 4, &app, 8, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == 0x0100000000021000ULL, "base id by application id");

    printf("{\"test\":\"svc_aoc\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
