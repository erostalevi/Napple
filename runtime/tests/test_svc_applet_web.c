/* Unit tests: the web (0x13) and offline web (0x17) library applets (applet_web_core.c): WebArgs TLV parse, offline path resolution, return value, and
 * dispatch through libapplet_register_handler via the real ILibraryAppletAccessor of applet.c. */
#include "../src/services/applet.c"
#include "svc_test.h"
#include "../src/services/applet_web_core.c"
#ifdef __linux__
const uint8_t g_user_id[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
void host_display_version(char out[16]) { snprintf(out, 16, "1.0.0"); }
uint64_t host_desired_language(void) { return 0x53552d6e65ULL; }
void ipc_trace_guest(unsigned n) { (void)n; }
void napple_display_game_size(uint32_t w, uint32_t h, uint32_t *gw, uint32_t *gh) { *gw = w; *gh = h; }
int frontend_text_input(const char *a, const char *b, const char *c, const char *d, unsigned e, unsigned f, int g, int h, char *out, unsigned cap) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; if (cap) out[0] = 0; return 0; }
size_t napple_utf16_to_utf8(const uint16_t *u, size_t n, char *out, size_t cap) { size_t k = 0; for (size_t i = 0; i < n && k + 1 < cap; i++) out[k++] = (char)u[i]; if (cap) out[k] = 0; return k; }
size_t napple_utf8_to_utf16(const char *s, uint16_t *out, size_t cap) { size_t k = 0; for (; s[k] && k < cap; k++) out[k] = (uint8_t)s[k]; return k; }
void frontend_alert(const char *t, const char *m) { (void)t; (void)m; }
int frontend_connected_pads(void) { return 1; }
#endif


/* CreateLibraryApplet(id, mode) on ILibraryAppletCreator */
static IpcObject *create_applet(IpcObject *lac, uint32_t id) {
    uint32_t in[2] = {id, 0}; IpcCall c; svc_call(lac, 0, in, 8, &c); return c.nout_objects ? c.out_objects[0] : NULL;
}
/* PushInData of an IStorage holding `size` bytes (data may be NULL for zeros) */
static void push(IpcObject *la, const uint8_t *data, uint64_t size) {
    IpcObject *st = storage_new(size); if (data) memcpy(((Storage *)st->state)->data, data, size);
    IpcCall c; memset(&c, 0, sizeof c); c.in_objects[0] = st; c.nin_objects = 1; svc_call_c(la, 100, &c); CHECK(c.result == 0, "PushInData");
}
/* Start, then GetResult; returns the result and, when the applet produced output, its bytes in out (size returned via *n) */
static uint32_t run(IpcObject *la, uint8_t *out, uint64_t *n) {
    IpcCall c; svc_call(la, 10, NULL, 0, &c); CHECK(c.result == 0, "Start");
    svc_call(la, 1, NULL, 0, &c); CHECK(c.out[0] == 1, "IsCompleted after Start");
    svc_call(la, 30, NULL, 0, &c); uint32_t r = c.result;
    svc_call(la, 101, NULL, 0, &c);
    if (c.result == 0 && c.nout_objects) { Storage *s = c.out_objects[0]->state; *n = s->size; memcpy(out, s->data, s->size); } else *n = 0;
    return r;
}


#include <unistd.h>
#include <sys/stat.h>
/* WebArgs builder: header { u16 entries; u16 pad; u32 shim } + TLVs { u16 type; u16 size; u32 pad } + data */
static size_t tlv(uint8_t *b, size_t off, uint16_t type, const void *d, uint16_t n) {
    memcpy(b + off, &type, 2); memcpy(b + off + 2, &n, 2); memset(b + off + 4, 0, 4); memcpy(b + off + 8, d, n); return off + 8 + n;
}
static size_t hdr(uint8_t *b, uint16_t entries, uint32_t shim) { memset(b, 0, 8); memcpy(b, &entries, 2); memcpy(b + 4, &shim, 4); return 8; }

static int g_open_calls, g_open_offline, g_open_ret;
static char g_open_url[0x1100];
static int open_hook(const char *url, int offline, char *last, size_t cap) {
    g_open_calls++; g_open_offline = offline; snprintf(g_open_url, sizeof g_open_url, "%s", url);
    snprintf(last, cap, "%s/final", url);
    return g_open_ret;
}
static void wfile(const char *dir, const char *rel, const char *txt) {
    char p[0x400]; snprintf(p, sizeof p, "%s/%s", dir, rel);
    FILE *f = fopen(p, "w"); if (f) { fputs(txt, f); fclose(f); }
}

int main(void) {
    uint8_t b[0x1400]; WebArgs a;
    /* ---- TLV parse ---- */
    size_t o = hdr(b, 3, WEB_SHIM_WEB);
    o = tlv(b, o, WEB_TLV_URL, "https://example.com/a?b=1\0", 26);
    o = tlv(b, o, WEB_TLV_CALLBACK_URL, "cb://done", 10);
    uint8_t shot = 1; o = tlv(b, o, WEB_TLV_SCREENSHOT_ENABLED, &shot, 1);
    web_args_parse(b, o, &a);
    CHECK(a.valid && a.shim_kind == WEB_SHIM_WEB && a.entries == 3, "header: shim %u entries %u", a.shim_kind, a.entries);
    CHECK(a.has_url && !strcmp(a.url, "https://example.com/a?b=1"), "url: %s", a.url);
    CHECK(a.has_callback && !strcmp(a.callback_url, "cb://done") && a.screenshot_enabled, "callback url and screenshot flag");
    o = hdr(b, 3, WEB_SHIM_OFFLINE);
    o = tlv(b, o, WEB_TLV_DOCUMENT_PATH, "offline:/manual/index.html", 27);
    uint32_t kind = WEB_DOC_HTML_PAGE; o = tlv(b, o, WEB_TLV_DOCUMENT_KIND, &kind, 4);
    uint64_t tid = 0x0100123400005000ULL; o = tlv(b, o, WEB_TLV_APPLICATION_ID, &tid, 8);
    web_args_parse(b, o, &a);
    CHECK(a.shim_kind == WEB_SHIM_OFFLINE && a.has_document_path && !strcmp(a.document_path, "offline:/manual/index.html") && a.document_kind == 1 && a.application_id == tid, "offline args");
    /* an unknown tag is skipped, a truncated entry stops the walk */
    o = hdr(b, 3, WEB_SHIM_WEB); uint8_t z[4] = {1, 2, 3, 4}; o = tlv(b, o, 0x77, z, 4); o = tlv(b, o, WEB_TLV_URL, "http://x\0", 9);
    web_args_parse(b, o, &a); CHECK(a.valid && a.has_url && !strcmp(a.url, "http://x"), "unknown tag skipped");
    web_args_parse(b, o - 4, &a); CHECK(a.valid && !a.has_url, "truncated entry stops the walk");
    web_args_parse(b, 4, &a); CHECK(!a.valid, "short storage invalid");
    web_args_parse(NULL, 0, &a); CHECK(!a.valid, "NULL storage invalid");

    /* ---- return value ---- */
    uint8_t rv[WEB_RETURN_SIZE]; web_return_encode(WEB_EXIT_LAST_URL, "http://last/", rv);
    uint32_t er; uint64_t ln; memcpy(&er, rv, 4); memcpy(&ln, rv + 0x1008, 8);
    CHECK(er == WEB_EXIT_LAST_URL && ln == 12 && !memcmp(rv + 8, "http://last/", 13), "return value layout");

    /* ---- offline path resolution ---- */
    char root[] = "/tmp/napple_web_XXXXXX"; CHECK(mkdtemp(root) != NULL, "mkdtemp");
    char sub[0x400], res[0x400]; snprintf(sub, sizeof sub, "%s/manual", root); mkdir(sub, 0755);
    snprintf(sub, sizeof sub, "%s/my dir", root); mkdir(sub, 0755);
    wfile(root, "manual/index.html", "<html>m</html>"); wfile(root, "manual/p2.html", "x"); wfile(root, "my dir/a.html", "x");
    char exp[0x400];
    CHECK(web_offline_resolve(root, "offline:/manual/p2.html", res, sizeof res) == 0, "resolve offline: prefix");
    snprintf(exp, sizeof exp, "%s/manual/p2.html", root); CHECK(!strcmp(res, exp), "resolved path %s", res);
    CHECK(web_offline_resolve(root, "manual/p2.html?x=1#top", res, sizeof res) == 0 && !strcmp(res, exp), "query and fragment dropped");
    CHECK(web_offline_resolve(root, "/manual/", res, sizeof res) == 0, "trailing slash -> index.html");
    snprintf(exp, sizeof exp, "%s/manual/index.html", root); CHECK(!strcmp(res, exp), "index.html default");
    CHECK(web_offline_resolve(root, "manual", res, sizeof res) == 0 && !strcmp(res, exp), "directory -> index.html");
    wfile(root, "my dir/b c.html", "x");
    CHECK(web_offline_resolve(root, "my%20dir/b%20c.html", res, sizeof res) == 0, "percent-encoded names decode");
    CHECK(web_offline_resolve(root, "%2e%2e/x", res, sizeof res) == -1 && web_offline_resolve(root, "manual/%2E%2e/%2e%2E/x", res, sizeof res) == -1, "encoded dotdot rejected");
    CHECK(web_offline_resolve(root, "manual%5cp2.html", res, sizeof res) == -1 && web_offline_resolve(root, "manual%00/p2.html", res, sizeof res) == -1, "encoded backslash and NUL rejected");
    CHECK(web_offline_resolve(root, "manual/p2%2ehtml", res, sizeof res) == 0 && web_offline_resolve(root, "manual/p%", res, sizeof res) == -2, "a lone percent stays literal");
    { char deep[0x400]; snprintf(deep, sizeof deep, "%s/a", root); mkdir(deep, 0755); strcat(deep, "/b"); mkdir(deep, 0755); strcat(deep, "/c"); mkdir(deep, 0755);
      wfile(root, "a/b/c/index.html", "x"); wfile(root, "a/b/c/page.html", "x");
      CHECK(web_offline_resolve(root, "offline:/a/b/c/page.html", res, sizeof res) == 0, "nested path resolves");
      CHECK(web_offline_resolve(root, "a/b/c", res, sizeof res) == 0 && web_offline_resolve(root, "a/b/c/", res, sizeof res) == 0, "nested directory -> index.html");
      CHECK(web_offline_resolve(root, "a/b/missing/page.html", res, sizeof res) == -2, "missing nested directory");
      char link[0x400], outside[0x400]; snprintf(link, sizeof link, "%s/a/escape", root); snprintf(outside, sizeof outside, "/tmp/napple_web_out_XXXXXX"); CHECK(mkdtemp(outside) != NULL, "mkdtemp outside"); wfile(outside, "secret.html", "x");
      if (symlink(outside, link) == 0) CHECK(web_offline_resolve(root, "a/escape/secret.html", res, sizeof res) == -1, "a symlink out of the tree is refused");
      else CHECK(1, "symlink unavailable"); }
    CHECK(web_offline_resolve(root, "../etc/passwd", res, sizeof res) == -1, "dotdot rejected");
    CHECK(web_offline_resolve(root, "manual/../../x", res, sizeof res) == -1, "nested dotdot rejected");
    CHECK(web_offline_resolve(root, "manual\\p2.html", res, sizeof res) == -1, "backslash rejected");
    CHECK(web_offline_resolve(root, "manual/missing.html", res, sizeof res) == -2, "missing file");
    CHECK(web_offline_resolve(root, "manual/p2.html", res, 8) == -3, "small buffer");
    CHECK(web_offline_resolve(NULL, "a", res, sizeof res) == -1, "no root");
    web_file_url("/a b/c.html", res, sizeof res); CHECK(!strcmp(res, "file:///a%20b/c.html"), "file url: %s", res);

    /* ---- dispatch through the hook, via the accessor ---- */
    libapplet_clear_handlers();
    CHECK(applet_web_register() == 0, "register web + offline web");
    CHECK(libapplet_find_handler(0x13) == applet_web_handler && libapplet_find_handler(0x17) == applet_web_handler, "both ids registered");
    IpcObject *oe = oe_open(); IpcCall c;
    svc_call(oe, 0, NULL, 0, &c); IpcObject *proxy = c.out_objects[0];
    svc_call(proxy, 11, NULL, 0, &c); IpcObject *lac = c.out_objects[0];
    uint8_t out[0x1100]; uint64_t n; uint8_t common[0x10] = {1, 0, 0, 0, 0x10, 0, 0, 0};
    applet_ui_set_open_web(NULL);

    /* web, no frontend hook: the user leaves at once on the requested page */
    IpcObject *la = create_applet(lac, 0x13); CHECK(la != NULL, "CreateLibraryApplet 0x13");
    o = hdr(b, 1, WEB_SHIM_WEB); o = tlv(b, o, WEB_TLV_URL, "https://example.com/\0", 21);
    push(la, common, sizeof common); push(la, b, o);
    uint32_t r = run(la, out, &n);
    CHECK(r == 0 && n == WEB_RETURN_SIZE, "web: result %u out %llu", r, (unsigned long long)n);
    memcpy(&er, out, 4); CHECK(er == WEB_EXIT_REQUESTED && !strcmp((char *)out + 8, "https://example.com/"), "web default: requested exit, last url");

    /* web with a hook */
    applet_ui_set_open_web(open_hook); g_open_ret = WEB_EXIT_CALLBACK_URL;
    la = create_applet(lac, 0x13); push(la, common, sizeof common); push(la, b, o); r = run(la, out, &n);
    memcpy(&er, out, 4);
    CHECK(g_open_calls == 1 && !g_open_offline && !strcmp(g_open_url, "https://example.com/"), "hook called with the url");
    CHECK(er == WEB_EXIT_CALLBACK_URL && !strcmp((char *)out + 8, "https://example.com//final"), "hook exit reason and last url returned");
    g_open_ret = -1; la = create_applet(lac, 0x13); push(la, common, sizeof common); push(la, b, o); run(la, out, &n);
    memcpy(&er, out, 4); CHECK(er == WEB_EXIT_ERROR, "hook failure -> error exit");

    /* scripted answers (NAPPLE_WEB_ANSWER) skip the hook and leave on the opened page */
    CHECK(applet_web_script(NULL) == -2 && applet_web_script("") == -2 && applet_web_script("x") == -2, "web script: unset / unknown");
    CHECK(applet_web_script("back") == WEB_EXIT_BACK_BUTTON && applet_web_script("closed") == WEB_EXIT_WINDOW_CLOSED && applet_web_script("requested") == WEB_EXIT_REQUESTED && applet_web_script("error") == -1 && applet_web_script("3") == 3, "web script values");
    { int calls = g_open_calls;
      setenv("NAPPLE_WEB_ANSWER", "closed", 1);
      la = create_applet(lac, 0x13); push(la, common, sizeof common); push(la, b, o); run(la, out, &n); memcpy(&er, out, 4);
      CHECK(g_open_calls == calls && er == WEB_EXIT_WINDOW_CLOSED && !strcmp((char *)out + 8, "https://example.com/"), "web NAPPLE_WEB_ANSWER=closed: hook skipped, exit reason, last url = opened page");
      setenv("NAPPLE_WEB_ANSWER", "error", 1);
      la = create_applet(lac, 0x13); push(la, common, sizeof common); push(la, b, o); run(la, out, &n); memcpy(&er, out, 4);
      CHECK(er == WEB_EXIT_ERROR, "web NAPPLE_WEB_ANSWER=error");
      unsetenv("NAPPLE_WEB_ANSWER"); }
    applet_web_set_html_root("/tmp/x"); CHECK(!strcmp(applet_web_html_root(), "/tmp/x"), "html root getter"); applet_web_set_html_root(NULL);

    /* web without a url -> error exit, no hook call */
    int before = g_open_calls; o = hdr(b, 0, WEB_SHIM_WEB);
    la = create_applet(lac, 0x13); push(la, common, sizeof common); push(la, b, o); run(la, out, &n);
    memcpy(&er, out, 4); CHECK(er == WEB_EXIT_ERROR && g_open_calls == before, "no url: error exit, hook untouched");

    /* offline web (0x17): document resolved under the html root and handed to the hook as a file:// url */
    applet_web_set_html_root(root); g_open_ret = WEB_EXIT_BACK_BUTTON;
    o = hdr(b, 2, WEB_SHIM_OFFLINE); o = tlv(b, o, WEB_TLV_DOCUMENT_PATH, "offline:/manual/p2.html\0", 24);
    kind = WEB_DOC_HTML_PAGE; o = tlv(b, o, WEB_TLV_DOCUMENT_KIND, &kind, 4);
    la = create_applet(lac, 0x17); CHECK(la != NULL, "CreateLibraryApplet 0x17"); push(la, common, sizeof common); push(la, b, o); r = run(la, out, &n);
    snprintf(exp, sizeof exp, "file://%s/manual/p2.html", root);
    CHECK(r == 0 && g_open_offline && !strcmp(g_open_url, exp), "offline: hook url %s", g_open_url);
    memcpy(&er, out, 4); CHECK(er == WEB_EXIT_BACK_BUTTON, "offline back button exit");
    /* missing document and an unavailable kind -> error exit */
    before = g_open_calls;
    o = hdr(b, 1, WEB_SHIM_OFFLINE); o = tlv(b, o, WEB_TLV_DOCUMENT_PATH, "nope.html\0", 10);
    la = create_applet(lac, 0x17); push(la, common, sizeof common); push(la, b, o); run(la, out, &n);
    memcpy(&er, out, 4); CHECK(er == WEB_EXIT_ERROR && g_open_calls == before, "offline missing document: error exit");
    o = hdr(b, 2, WEB_SHIM_OFFLINE); o = tlv(b, o, WEB_TLV_DOCUMENT_PATH, "manual/p2.html\0", 15); kind = WEB_DOC_LEGAL_INFO; o = tlv(b, o, WEB_TLV_DOCUMENT_KIND, &kind, 4);
    la = create_applet(lac, 0x17); push(la, common, sizeof common); push(la, b, o); run(la, out, &n);
    memcpy(&er, out, 4); CHECK(er == WEB_EXIT_ERROR, "legal information kind: error exit");
    /* unregistered ids still take the old path */
    libapplet_clear_handlers(); applet_ui_set_open_web(NULL);
    before = g_open_calls; la = create_applet(lac, 0x13); push(la, common, sizeof common); push(la, b, o); r = run(la, out, &n);
    CHECK(g_open_calls == before, "no handler: the hook is not reached");

    { char cmd[0x500]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", root); (void)!system(cmd); }
    printf("{\"test\":\"svc_applet_web\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed ? 1 : 0;
}
