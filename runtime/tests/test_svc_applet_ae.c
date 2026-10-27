/* Unit tests: appletAE (applet_ae.c), the system-applet side a firmware applet process talks to: the storage files of the caller (in/iin), the applet's answers
 * (out/iout), the theme storage and the terminate result. applet.c is #included too (it owns the storage objects applet_ae.c builds on). */
#include "../src/services/applet.c"
#include "../src/services/applet_ae.c"
#include "svc_test.h"
#ifdef __linux__
const uint8_t g_user_id[16] = {1};
void host_display_version(char out[16]) { snprintf(out, 16, "1.0.0"); }
uint64_t host_desired_language(void) { return 0x53552d6e65ULL; }
void ipc_trace_guest(unsigned n) { (void)n; }
void napple_display_game_size(uint32_t w, uint32_t h, uint32_t *gw, uint32_t *gh) { *gw = w; *gh = h; }
int frontend_text_input(const char *a, const char *b, const char *c, const char *d, unsigned e, unsigned f, int g, int h, char *out, unsigned cap) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; if (cap) out[0] = 0; return 0; }
size_t napple_utf16_to_utf8(const uint16_t *u, size_t n, char *out, size_t cap) { size_t k = 0; for (size_t i = 0; i < n && k + 1 < cap; i++) out[k++] = (char)u[i]; if (cap) out[k] = 0; return k; }
size_t napple_utf8_to_utf16(const char *s, uint16_t *out, size_t cap) { size_t k = 0; for (; s[k] && k < cap; k++) out[k] = (uint8_t)s[k]; return k; }
void frontend_alert(const char *t, const char *m) { (void)t; (void)m; }
int frontend_connected_pads(void) { return 1; }
void hid_apply_system_common_policy(void) {}
void *host_symbol_lookup(const char *n) { (void)n; return NULL; }
void host_symbols_register(const HostSymbol *s, size_t n) { (void)s; (void)n; }
void runtime_fail(const char *fmt, ...) { (void)fmt; }
void summary_note(const char *fmt, ...) { (void)fmt; }
#endif

static IpcObject *get(IpcObject *o, unsigned cmd) { IpcCall c; svc_call(o, cmd, NULL, 0, &c); return c.nout_objects ? c.out_objects[0] : NULL; }
static void write_f(const char *dir, const char *name, const void *d, size_t n) { char p[300]; snprintf(p, sizeof p, "%s/%s", dir, name); FILE *f = fopen(p, "wb"); fwrite(d, 1, n, f); fclose(f); }

int main(void) {
    char root[96], in_dir[128], out_dir[128], cmd[400];
    snprintf(root, sizeof root, "/tmp/napple-applet-ae-%d", (int)getpid());
    snprintf(in_dir, sizeof in_dir, "%s/in", root); snprintf(out_dir, sizeof out_dir, "%s/out", root);
    snprintf(cmd, sizeof cmd, "mkdir -p %s %s", in_dir, out_dir); CHECK(system(cmd) == 0, "scaffolding");
    setenv("NAPPLE_APPLET_IN", in_dir, 1); setenv("NAPPLE_APPLET_OUT", out_dir, 1); setenv("NAPPLE_APPLET_ID", "0x11", 1); setenv("NAPPLE_APPLET_MODE", "1", 1);
    uint8_t common[8] = {1, 0, 0, 0, 8, 0, 0, 0}, a0[16] = {9, 9}, i0[4] = {0xC0, 0xDE}, i1[2] = {0xEE};
    write_f(in_dir, "in0.bin", common, 8); write_f(in_dir, "in1.bin", a0, 16); write_f(in_dir, "iin0.bin", i0, 4); write_f(in_dir, "iin1.bin", i1, 2);

    IpcObject *ae = ae_open(); IpcCall c;
    svc_call(ae, 201, NULL, 0, &c); IpcObject *proxy = c.out_objects[0];
    CHECK(g_applet_kind == 3, "the proxy marks this process as a library applet");
    IpcObject *sa = get(proxy, 20), *pw = get(proxy, 10), *acf = get(proxy, 21);
    CHECK(sa && pw && acf, "proxy hands out the self accessor, winding controller, common functions");

    svc_call(sa, 11, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 0x11 && SVC_OUT(&c, uint32_t, 4) == 1, "GetLibraryAppletInfo reports the id and mode");
    svc_call(sa, 5, NULL, 0, &c); CHECK(event_is_signaled(c.out_copy[0]), "pop-in event signalled while storages remain");
    svc_call(sa, 0, NULL, 0, &c); CHECK(c.result == 0 && c.nout_objects == 1 && ((Storage *)c.out_objects[0]->state)->size == 16, "PopInData skips the common arguments and pops the applet's own storage");
    svc_call(sa, 0, NULL, 0, &c); CHECK(c.result == 0x680, "PopInData: no more data");

    svc_call(sa, 6, NULL, 0, &c); CHECK(event_is_signaled(c.out_copy[0]), "interactive pop-in event signalled while storages remain");
    svc_call(sa, 2, NULL, 0, &c); CHECK(c.result == 0 && ((Storage *)c.out_objects[0]->state)->size == 4 && ((Storage *)c.out_objects[0]->state)->data[0] == 0xC0, "PopInteractiveInData #0");
    svc_call(sa, 2, NULL, 0, &c); CHECK(c.result == 0 && ((Storage *)c.out_objects[0]->state)->size == 2, "PopInteractiveInData #1");
    svc_call(sa, 2, NULL, 0, &c); CHECK(c.result == 0x680, "PopInteractiveInData: empty");
    svc_call(sa, 6, NULL, 0, &c); CHECK(!event_is_signaled(c.out_copy[0]), "interactive pop-in event clear once drained");

    IpcObject *st = applet_storage_make("hello", 5);
    IpcCall po; memset(&po, 0, sizeof po); po.in_objects[0] = st; po.nin_objects = 1; svc_call_c(sa, 1, &po); CHECK(po.result == 0, "PushOutData");
    IpcObject *st2 = applet_storage_make("ia", 2);
    memset(&po, 0, sizeof po); po.in_objects[0] = st2; po.nin_objects = 1; svc_call_c(sa, 3, &po); CHECK(po.result == 0, "PushInteractiveOutData");
    { char p[300]; uint64_t n; snprintf(p, sizeof p, "%s/out0.bin", out_dir); uint8_t *b = read_file(p, &n); CHECK(b && n == 5 && !memcmp(b, "hello", 5), "out0.bin holds the output storage"); free(b);
      snprintf(p, sizeof p, "%s/iout0.bin", out_dir); b = read_file(p, &n); CHECK(b && n == 2 && !memcmp(b, "ia", 2), "iout0.bin holds the interactive output storage"); free(b);
      snprintf(p, sizeof p, "%s/out1.bin", out_dir); struct stat sb; CHECK(stat(p, &sb) != 0, "the interactive output did not take an out<N> slot"); }
    memset(&po, 0, sizeof po); svc_call_c(sa, 3, &po); CHECK(po.result != 0, "PushInteractiveOutData without a storage fails");

    svc_call(sa, 13, NULL, 0, &c); CHECK(c.out[0] == 0, "CanUseApplicationCore: no");
    svc_call(sa, 16, NULL, 0, &c); CHECK(SVC_OUT(&c, uint32_t, 0) == 4, "GetMainAppletStorageId: NAND user");
    svc_call(sa, 14, NULL, 0, &c); CHECK(c.result == 0 && c.out_size == 0x18, "GetCallerAppletIdentityInfo");
    svc_call(pw, 0, NULL, 0, &c); CHECK(c.result == 0, "GetLaunchReason");

    uint32_t tr = 0x2a5; svc_call(acf, 0, &tr, 4, &c); CHECK(terminate_result == 0x2a5, "SetTerminateResult is kept for the exit");
    svc_call(acf, 21, NULL, 0, &c); CHECK(c.result == 0x680, "TryPopFromAppletBoundChannel: empty");
    { IpcCall r; memset(&r, 0, sizeof r); static uint8_t buf[0x40]; svc_set_out_buf(&r, buf, sizeof buf); svc_call_c(acf, 10, &r); CHECK(r.result == 0x680, "ReadThemeStorage before any write"); }
    { uint8_t theme[8] = {1, 2, 3, 4, 5, 6, 7, 8}; IpcCall w; memset(&w, 0, sizeof w); svc_set_in_buf(&w, theme, sizeof theme); svc_call_c(acf, 11, &w); CHECK(w.result == 0, "WriteThemeStorage");
      IpcCall r; memset(&r, 0, sizeof r); static uint8_t buf[0x40]; svc_set_out_buf(&r, buf, sizeof buf); svc_call_c(acf, 10, &r); CHECK(r.result == 0 && buf[3] == 4 && buf[7] == 8, "ReadThemeStorage reads back what was written"); }

    snprintf(cmd, sizeof cmd, "rm -rf %s", root); (void)!system(cmd);
    printf("{\"test\":\"svc_applet_ae\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
