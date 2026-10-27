/* Unit tests: ns core (language mapping/selection, free space) and the ns:am2 / ns:ec tables. */
#include "../src/services/ns.c"
#include "svc_test.h"
#include <unistd.h>   /* mkdtemp on macOS */
#ifdef __linux__
const char *host_save_dir(void) { return "/tmp"; }
uint64_t host_desired_language(void) { return ns_language_code(NS_LANG_GERMAN); }
#endif

static IpcObject *doc_obj(IpcObject *root) { IpcCall k; svc_call(root, 7999, NULL, 0, &k); return k.out_objects[0]; }

int main(void) {
    char root[] = "/tmp/napple-nssvc-XXXXXX"; g_cfg.save_root = mkdtemp(root); snprintf(g_cfg.title_id, sizeof g_cfg.title_id, "0100ABCD12345678");   /* host_save_dir() on macOS */
    CHECK(ns_language_from_code(ns_language_code(NS_LANG_JAPANESE)) == NS_LANG_JAPANESE, "ja round trip");
    for (int i = 0; i < NS_LANG_COUNT; i++) CHECK(ns_language_code(i) && ns_language_from_code(ns_language_code(i)) == i, "language %d round trip", i);
    CHECK(ns_language_from_code(0x1234) == -1 && ns_language_code(99) == 0, "unknown language");
    uint64_t jp = ns_language_code(NS_LANG_JAPANESE); char b[9] = {0}; memcpy(b, &jp, 8); CHECK(!strcmp(b, "ja"), "code is ASCII text, NUL padded");
    uint32_t all = (1u << NS_LANG_COUNT) - 1;
    CHECK(ns_select_language(all, ns_language_code(NS_LANG_GERMAN)) == NS_LANG_GERMAN, "supported language is chosen");
    CHECK(ns_select_language(1u << NS_LANG_AMERICAN_ENGLISH, ns_language_code(NS_LANG_BRITISH_ENGLISH)) == NS_LANG_AMERICAN_ENGLISH, "en-GB falls back to en-US");
    CHECK(ns_select_language(1u << NS_LANG_SPANISH, ns_language_code(NS_LANG_LATIN_AMERICAN_SPANISH)) == NS_LANG_SPANISH, "es-419 falls back to es");
    CHECK(ns_select_language((1u << NS_LANG_JAPANESE) | (1u << NS_LANG_AMERICAN_ENGLISH), ns_language_code(NS_LANG_GERMAN)) == NS_LANG_AMERICAN_ENGLISH, "unsupported: English default");
    CHECK(ns_select_language((1u << NS_LANG_JAPANESE) | (1u << NS_LANG_KOREAN), ns_language_code(NS_LANG_GERMAN)) == NS_LANG_JAPANESE, "unsupported, no English: lowest supported");
    CHECK(ns_select_language(0, ns_language_code(NS_LANG_JAPANESE)) == -1, "nothing supported");
    CHECK(ns_total_space("/tmp") > 0 && ns_free_space("/tmp") <= ns_total_space("/tmp") && ns_free_space("/nonexistent-dir-xyz") == 0, "statvfs space");

    IpcCall c; IpcObject *am2 = am2_open(), *ec = ec_open();
    CHECK(ec->ncmds == am2->ncmds, "ns:ec shares the getter table");
    svc_call(am2, 7996, NULL, 0, &c); CHECK(c.nout_objects == 1, "GetApplicationManagerInterface");
    IpcObject *am = c.out_objects[0];
    uint32_t sup = (1u << NS_LANG_GERMAN) | (1u << NS_LANG_FRENCH);
    svc_call(am, 401, &sup, 4, &c); CHECK(c.result == 0 && c.out_size == 1 && c.out[0] == ns_select_language(sup, host_desired_language()), "GetApplicationDesiredLanguage follows the host language (German on Linux)");
#ifdef __linux__
    CHECK(c.out[0] == NS_LANG_GERMAN, "mock host language is German");
#endif
    uint32_t none = 0; svc_call(am, 401, &none, 4, &c); CHECK(c.result != 0, "no supported language is an error");
    uint8_t l = NS_LANG_FRENCH; svc_call(am, 402, &l, 1, &c); CHECK(c.out_size == 8 && SVC_OUT(&c, uint64_t, 0) == ns_language_code(NS_LANG_FRENCH), "ConvertApplicationLanguageToLanguageCode");
    uint64_t code = ns_language_code(NS_LANG_KOREAN); svc_call(am, 403, &code, 8, &c); CHECK(c.out[0] == NS_LANG_KOREAN, "ConvertLanguageCodeToApplicationLanguage");
    svc_call(am, 400, NULL, 0, &c); CHECK(c.result == HZ_R_NS_APPLICATION_NOT_FOUND, "GetApplicationControlData: not found without control.nacp");
    svc_call(am, 0, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "no application records");
    svc_call(am, 1, NULL, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 1, "GenerateApplicationRecordCount = the running title");
    { uint8_t rec[0x30] = {0}; int32_t off = 0; c = (IpcCall){0}; c.in = (const uint8_t *)&off; c.in_size = 4; svc_set_out_buf(&c, rec, sizeof rec); svc_call_c(am, 0, &c);
      CHECK(SVC_OUT(&c, int32_t, 0) == 1 && *(uint64_t *)rec == 0x0100ABCD12345678ull && rec[8] == 3, "ListApplicationRecord returns the running title");
      off = 1; c = (IpcCall){0}; c.in = (const uint8_t *)&off; c.in_size = 4; svc_set_out_buf(&c, rec, sizeof rec); svc_call_c(am, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "offset past the only record: 0");
      off = 0; c = (IpcCall){0}; c.in = (const uint8_t *)&off; c.in_size = 4; svc_set_out_buf(&c, rec, 8); svc_call_c(am, 0, &c); CHECK(SVC_OUT(&c, int32_t, 0) == 0, "buffer too small for a record: 0"); }
    svc_call(am, 2, NULL, 0, &c); CHECK(c.nout_copy == 1 && !event_is_signaled(c.out_copy[0]), "record update event never fires");
    CHECK(ns_parse_title_id("0100ABCD12345678") == 0x0100ABCD12345678ull && ns_parse_title_id("0100ABCD1234567") == 0 && ns_parse_title_id("0100ABCD1234567G") == 0 && ns_parse_title_id(NULL) == 0, "title id parsing");
    svc_call(am2, 7998, NULL, 0, &c); IpcObject *cm = c.out_objects[0];
    uint8_t st = 4; svc_call(cm, 48, &st, 1, &c); uint64_t fr = SVC_OUT(&c, uint64_t, 0); svc_call(cm, 47, &st, 1, &c); uint64_t tot = SVC_OUT(&c, uint64_t, 0);
    CHECK(tot > 0 && fr <= tot, "user storage reports the host volume");
    st = 3; svc_call(cm, 47, &st, 1, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == 0, "system storage: 0");
    svc_call(cm, 11, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 0x80, "CalculateApplicationOccupiedSize: zeroes");
    svc_call(am2, 7992, NULL, 0, &c); IpcObject *eci = c.out_objects[0];
    svc_call(eci, 0, NULL, 0, &c); CHECK(c.result == HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED, "e-commerce needs a network");
    svc_call(am2, 7993, NULL, 0, &c); IpcObject *ver = c.out_objects[0];
    uint64_t app = 1; svc_call(ver, 0, &app, 8, &c); CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == 0, "GetLaunchRequiredVersion = 0");
    IpcObject *vm = vm_open();
    svc_call(vm, 1200, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 1 && c.out[0] == 0, "ns:vm NeedsUpdateVulnerability = false");
    svc_call(vm, 1201, NULL, 0, &c); CHECK(c.result == 0, "ns:vm UpdateSafeSystemVersionForDebug accepted");
    { /* control.nacp next to the data directory */
        char dd[] = "/tmp/napple-nsdata-XXXXXX"; g_cfg.data_dir = mkdtemp(dd);
        uint8_t nb[NS_NACP_SIZE]; svc_call(am, 400, NULL, 0, &c); CHECK(c.result == HZ_R_NS_APPLICATION_NOT_FOUND && !ns_control_load(g_cfg.data_dir, nb), "no control.nacp: not found");
        svc_call(doc_obj(am2), 21, (uint8_t[16]){NS_CONTENT_CONTROL}, 16, &c); CHECK(c.result == HZ_R_NS_APPLICATION_NOT_FOUND, "GetApplicationContentPath: not found without the file");
        char fp[300]; snprintf(fp, sizeof fp, "%s/control.nacp", dd); FILE *f = fopen(fp, "wb"); uint8_t src[0x300]; for (unsigned i = 0; i < sizeof src; i++) src[i] = (uint8_t)(i * 7 + 1); fwrite(src, 1, sizeof src, f); fclose(f);
        CHECK(ns_control_load(dd, nb) == NS_NACP_SIZE && !memcmp(nb, src, sizeof src) && nb[sizeof src] == 0 && nb[NS_NACP_SIZE - 1] == 0, "a short control.nacp is zero-padded to 0x4000");
        static uint8_t out[0x16000]; uint8_t in400[16] = {0}; uint64_t host = 0x0100ABCD12345678ull; memcpy(in400 + 8, &host, 8);
        memset(&c, 0, sizeof c); c.in = in400; c.in_size = 16; svc_set_out_buf(&c, out, sizeof out); svc_call_c(am, 400, &c);
        CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == NS_NACP_SIZE && !memcmp(out, src, sizeof src), "GetApplicationControlData returns the NACP and its size");
        uint64_t other = host + 1; memcpy(in400 + 8, &other, 8); memset(&c, 0, sizeof c); c.in = in400; c.in_size = 16; svc_set_out_buf(&c, out, sizeof out); svc_call_c(am, 400, &c);
        CHECK(c.result == HZ_R_NS_APPLICATION_NOT_FOUND, "another application id is unknown");
        uint8_t small[0x100]; memcpy(in400 + 8, &host, 8); memset(&c, 0, sizeof c); c.in = in400; c.in_size = 16; svc_set_out_buf(&c, small, sizeof small); svc_call_c(am, 400, &c);
        CHECK(c.result == 0 && SVC_OUT(&c, uint32_t, 0) == sizeof small && !memcmp(small, src, sizeof small), "a small output buffer gets a prefix and its size");
        uint8_t in21[16] = {NS_CONTENT_CONTROL}; memcpy(in21 + 8, &host, 8); char path[0x300]; memset(&c, 0, sizeof c); c.in = in21; c.in_size = 16; svc_set_out_buf(&c, path, sizeof path); svc_call_c(doc_obj(am2), 21, &c);
        CHECK(c.result == 0 && !strcmp(path, fp), "GetApplicationContentPath(Control) = <data_dir>/control.nacp");
        svc_call(doc_obj(am2), 23, in21, 16, &c); CHECK(c.result == 0, "ResolveApplicationContentPath(Control)");
        in21[0] = 0; svc_call(doc_obj(am2), 23, in21, 16, &c); CHECK(c.result == HZ_R_NS_APPLICATION_NOT_FOUND, "other content types are not found");
        unlink(fp); rmdir(dd); g_cfg.data_dir = NULL;
    }
    { char cmd[200]; snprintf(cmd, sizeof cmd, "rm -rf %s", root); (void)system(cmd); }
    printf("{\"test\":\"svc_ns\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
