/* Unit tests: set:fd store and iterator. */
#include "../src/services/setfd.c"
#include "svc_test.h"

static void put(IpcObject *o, const char *name, const char *key, const void *v, size_t n, IpcCall *c) {
    memset(c, 0, sizeof *c); svc_set_in_buf(c, name, strlen(name) + 1); svc_set_in_buf(c, key, strlen(key) + 1); svc_set_in_buf(c, v, n); svc_call_c(o, 2, c);
}
int main(void) {
    SetfdStore st; memset(&st, 0, sizeof st); uint8_t out[8];
    CHECK(setfd_set(&st, "g", "k", "\x01\x02", 2) == 0 && setfd_get(&st, "g", "k", out, 8) == 2 && out[1] == 2, "set and get");
    CHECK(setfd_set(&st, "g", "k", "\x09", 1) == 0 && setfd_get(&st, "g", "k", out, 8) == 1 && out[0] == 9, "set again replaces");
    CHECK(setfd_get(&st, "g", "nope", out, 8) == -1 && setfd_get(&st, "h", "k", out, 8) == -1, "absent");
    CHECK(setfd_set(&st, "", "k", "x", 1) == -1 && setfd_set(&st, "g", "", "x", 1) == -1, "empty names rejected");
    char big[SETFD_VALUE_MAX + 1] = {0}; CHECK(setfd_set(&st, "g", "big", big, sizeof big) == -2, "oversized value rejected");
    char longname[100]; memset(longname, 'a', 99); longname[99] = 0; CHECK(setfd_set(&st, longname, "k", "x", 1) == -1, "oversized name rejected");
    CHECK(setfd_reset(&st, "g", "k") == 1 && setfd_reset(&st, "g", "k") == 0 && setfd_get(&st, "g", "k", out, 8) == -1, "reset");
    SetfdStore full; memset(&full, 0, sizeof full); char key[8]; int last = 0;
    for (int i = 0; i < SETFD_MAX_ITEMS + 1; i++) { snprintf(key, sizeof key, "k%d", i); last = setfd_set(&full, "g", key, "x", 1); }
    CHECK(last == -3, "store full");
    char d[8]; CHECK(setfd_copy_string(d, 8, "abc\0zzz", 7) == 0 && !strcmp(d, "abc"), "copy stops at NUL");
    CHECK(setfd_copy_string(d, 4, "abcd", 4) == -1, "copy refuses to truncate");
    char raw[3] = {'a', 'b', 'c'}; CHECK(setfd_copy_string(d, 8, raw, 3) == 0 && !strcmp(d, "abc"), "unterminated buffer");

    memset(&store, 0, sizeof store);
    IpcObject *o = fd_open(); IpcCall c;
    put(o, "grp", "alpha", "1", 1, &c); CHECK(c.result == 0, "SetSettingsItemValue");
    put(o, "grp", "beta", "22", 2, &c); put(o, "other", "gamma", "3", 1, &c);
    CHECK(setfd_get(&store, "grp", "beta", out, 8) == 2, "value reached the store");
    memset(&c, 0, sizeof c); svc_set_in_buf(&c, "grp", 4); svc_call_c(o, 4, &c); CHECK(c.nout_objects == 1, "CreateSettingsItemKeyIterator");
    IpcObject *it = c.out_objects[0]; char kbuf[16];
    svc_call(it, 1, NULL, 0, &c); CHECK(SVC_OUT(&c, uint64_t, 0) == 6, "key size includes the NUL (\"alpha\")");
    memset(&c, 0, sizeof c); svc_set_out_buf(&c, kbuf, sizeof kbuf); svc_call_c(it, 2, &c); CHECK(!strcmp(kbuf, "alpha"), "first key");
    svc_call(it, 0, NULL, 0, &c); memset(&c, 0, sizeof c); svc_set_out_buf(&c, kbuf, sizeof kbuf); svc_call_c(it, 2, &c); CHECK(!strcmp(kbuf, "beta"), "second key; other groups are not listed");
    svc_call(it, 0, NULL, 0, &c); svc_call(it, 1, NULL, 0, &c); CHECK(c.result != 0, "past the end: error");
    memset(&c, 0, sizeof c); svc_set_in_buf(&c, "grp", 4); svc_set_in_buf(&c, "alpha", 6); svc_call_c(o, 3, &c);
    CHECK(c.result == 0 && setfd_get(&store, "grp", "alpha", out, 8) == -1, "ResetSettingsItemValue");
    memset(&c, 0, sizeof c); svc_call_c(o, 3, &c); CHECK(c.result == R_BAD_ARG, "missing buffers: invalid argument");
    printf("{\"test\":\"svc_setfd\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
