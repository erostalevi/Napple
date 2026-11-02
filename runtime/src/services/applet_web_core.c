#include "applet_web_core.h"
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#include "applet_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static AppletUiOpenWeb g_open_web;
void applet_ui_set_open_web(AppletUiOpenWeb fn) { g_open_web = fn; }
int applet_ui_open_web(const char *url, int offline, char *last_url, size_t cap) {
    if (!g_open_web) return -2;
    return g_open_web(url, offline, last_url, cap);
}

int applet_web_script(const char *spec) {
    if (!spec || !*spec) return -2;
    if (!strcmp(spec, "back")) return WEB_EXIT_BACK_BUTTON;
    if (!strcmp(spec, "closed")) return WEB_EXIT_WINDOW_CLOSED;
    if (!strcmp(spec, "requested")) return WEB_EXIT_REQUESTED;
    if (!strcmp(spec, "error")) return -1;
    if (spec[0] >= '0' && spec[0] <= '9') return atoi(spec);
    return -2;
}

static void copy_str(char *dst, size_t cap, const uint8_t *src, size_t n) {
    size_t k = 0;
    while (k < n && k + 1 < cap && src[k]) {
        dst[k] = (char)src[k];
        k++;
    }
    dst[k] = 0;
}

void web_args_parse(const uint8_t *data, size_t size, WebArgs *a) {
    memset(a, 0, sizeof *a);
    if (!data || size < 8) return;
    uint16_t entries;
    memcpy(&entries, data, 2);
    memcpy(&a->shim_kind, data + 4, 4);
    a->entries = entries;
    a->valid = 1;
    size_t off = 8;
    for (unsigned i = 0; i < entries; i++) {
        if (size - off < 8) break;
        uint16_t type, len;
        memcpy(&type, data + off, 2);
        memcpy(&len, data + off + 2, 2);
        off += 8;
        if (len > size - off) break;
        const uint8_t *p = data + off;
        switch (type) {
            case WEB_TLV_URL:
            case WEB_TLV_CALLBACKABLE_URL: /* a callbackable url is the page to show as well */
                if (type == WEB_TLV_URL || !a->has_url) copy_str(a->url, sizeof a->url, p, len), a->has_url = 1;
                break;
            case WEB_TLV_CALLBACK_URL:
                copy_str(a->callback_url, sizeof a->callback_url, p, len);
                a->has_callback = 1;
                break;
            case WEB_TLV_DOCUMENT_PATH:
                copy_str(a->document_path, sizeof a->document_path, p, len);
                a->has_document_path = 1;
                break;
            case WEB_TLV_DOCUMENT_KIND:
                if (len >= 4) memcpy(&a->document_kind, p, 4), a->has_document_kind = 1;
                break;
            case WEB_TLV_APPLICATION_ID:
                if (len >= 8) memcpy(&a->application_id, p, 8);
                break;
            case WEB_TLV_SCREENSHOT_ENABLED: a->screenshot_enabled = len >= 1 && p[0]; break;
            default: break;
        }
        off += len;
    }
}

static int resolve_under(const char *html_root, const char *base, const char *rel, char *out, size_t cap);
int web_offline_resolve(const char *html_root, const char *doc_path, char *out, size_t cap) {
    if (!html_root || !doc_path) return -1;
    const char *p = doc_path;
    if (!strncmp(p, "offline:", 8)) p += 8;
    while (*p == '/') p++;
    char rel[0x200];
    size_t n = strlen(p);
    if (n >= sizeof rel - 12) return -1;
    memcpy(rel, p, n + 1);
    /* drop a query or fragment (before decoding: an encoded %3F is part of a file name) */
    for (size_t i = 0; i < n; i++)
        if (rel[i] == '?' || rel[i] == '#') {
            rel[i] = 0;
            n = i;
            break;
        }
    /* percent-decode (a page link written "my%20page.html"); a decoded NUL is rejected, backslash and ".." are checked after decoding */
    {
        size_t w = 0;
        for (size_t i = 0; i < n; i++) {
            unsigned char ch = (unsigned char)rel[i];
            if (ch == '%' && i + 2 < n && isxdigit((unsigned char)rel[i + 1]) && isxdigit((unsigned char)rel[i + 2])) {
                char hx[3] = {rel[i + 1], rel[i + 2], 0};
                ch = (unsigned char)strtol(hx, NULL, 16);
                if (!ch) return -1;
                i += 2;
            }
            rel[w++] = (char)ch;
        }
        rel[w] = 0;
        n = w;
    }
    for (size_t i = 0; i < n; i++)
        if (rel[i] == '\\') return -1;
    /* reject ".." components */
    for (const char *s = rel; *s;) {
        const char *e = strchr(s, '/');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l == 2 && s[0] == '.' && s[1] == '.') return -1;
        s += l + (e ? 1 : 0);
        if (!e) break;
    }
    if (n == 0 || rel[n - 1] == '/') strcat(rel, "index.html");
    /* An OfflineHtmlPage path is relative to the html-document/ directory of the HtmlDocument romfs (a real manual: html-document/<name>.htdocs/
     * html/<RegionLang>/index.html). The build writes that romfs as it is, so look there first, then directly under the root (older trees, tests). */
    char doc_dir[0x400];
    struct stat st;
    int w = snprintf(doc_dir, sizeof doc_dir, "%s/html-document", html_root);
    int r = -2;
    if (w > 0 && (size_t)w < sizeof doc_dir && strncmp(rel, "html-document/", 14) && stat(doc_dir, &st) == 0 && S_ISDIR(st.st_mode))
        r = resolve_under(html_root, doc_dir, rel, out, cap);
    if (r == -2) r = resolve_under(html_root, html_root, rel, out, cap);
    return r;
}

/* <base>/<rel> (a directory -> its index.html), contained in html_root after symlinks. 0 ok, -1 bad, -2 missing, -3 out too small. */
static int resolve_under(const char *html_root, const char *base, const char *rel, char *out, size_t cap) {
    char path[0x400];
    int w = snprintf(path, sizeof path, "%s/%s", base, rel);
    if (w < 0 || (size_t)w >= sizeof path) return -1;
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (strlen(path) + 12 >= sizeof path) return -1;
        strcat(path, "/index.html");
    }
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -2;
    { /* a symlink inside the document tree must not lead out of it */
        char rp[PATH_MAX], rr[PATH_MAX];
        if (realpath(path, rp) && realpath(html_root, rr)) {
            size_t l = strlen(rr);
            if (strncmp(rp, rr, l) || (rp[l] && rp[l] != '/')) return -1;
        }
    }
    if (strlen(path) >= cap) return -3;
    strcpy(out, path);
    return 0;
}

int web_manual_path(const char *html_root, const char *lang, char *out, size_t cap) {
    if (!html_root || !cap) return -1;
    char dir[0x400];
    snprintf(dir, sizeof dir, "%s/html-document", html_root);
    DIR *d = opendir(dir);
    if (!d) return -2;
    const char *langs[3] = {lang && *lang ? lang : "USen", "USen", "EUen"};
    char best[0x200] = "", any[0x200] = "";
    struct dirent *e;
    while (!best[0] && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 8 || strcmp(e->d_name + l - 7, ".htdocs")) continue;
        for (int i = 0; i < 3 && !best[0]; i++) {
            char rel[0x200], path[0x700];
            snprintf(rel, sizeof rel, "%s/html/%s/index.html", e->d_name, langs[i]);
            snprintf(path, sizeof path, "%s/%s", dir, rel);
            if (access(path, R_OK) == 0) snprintf(best, sizeof best, "%s", rel);
        }
        if (!any[0]) { /* no wanted language: the first language directory with an index */
            char hd[0x700];
            snprintf(hd, sizeof hd, "%s/%s/html", dir, e->d_name);
            DIR *h = opendir(hd);
            struct dirent *le;
            while (h && !any[0] && (le = readdir(h))) {
                if (le->d_name[0] == '.') continue;
                char path[0xa00];
                snprintf(path, sizeof path, "%s/%s/index.html", hd, le->d_name);
                if (access(path, R_OK) == 0) snprintf(any, sizeof any, "%s/html/%s/index.html", e->d_name, le->d_name);
            }
            if (h) closedir(h);
        }
    }
    closedir(d);
    const char *r = best[0] ? best : any;
    if (!r[0]) return -2;
    if (strlen(r) >= cap) return -3;
    strcpy(out, r);
    return 0;
}

const char *web_manual_lang(const char *code) {
    static const char *const map[][2] = {{"ja", "JPja"},     {"en-US", "USen"}, {"fr", "EUfr"},      {"de", "EUde"},      {"it", "EUit"},   {"es", "EUes"},
                                         {"zh-CN", "CNzh"},  {"ko", "KRko"},    {"nl", "EUnl"},      {"pt", "EUpt"},      {"ru", "EUru"},   {"zh-TW", "TWzh"},
                                         {"en-GB", "EUen"},  {"fr-CA", "USfr"}, {"es-419", "USes"},  {"zh-Hans", "CNzh"}, {"zh-Hant", "TWzh"}, {"pt-BR", "USpt"}};
    for (unsigned i = 0; code && i < sizeof map / sizeof map[0]; i++)
        if (!strcmp(code, map[i][0])) return map[i][1];
    return "USen";
}

/* One TLV entry of a WebArgs storage. */
static size_t tlv_put(uint8_t *buf, size_t off, uint16_t type, const void *data, uint16_t len) {
    memcpy(buf + off, &type, 2);
    memcpy(buf + off + 2, &len, 2);
    memset(buf + off + 4, 0, 4);
    memcpy(buf + off + 8, data, len);
    return off + 8 + len;
}
int applet_web_open_manual(const char *lang, uint32_t *exit_reason, char *last_url, size_t cap) {
    const char *root = applet_web_html_root();
    char rel[0x200];
    if (!root || web_manual_path(root, lang, rel, sizeof rel)) return -1;
    /* The same storages the HOME menu's offline applet gets: CommonArguments, then WebArgs {shim offline; document kind HTML page; document path}. */
    uint8_t common[0x20] = {0}, args[0x400] = {0};
    uint16_t entries = 2;
    uint32_t shim = WEB_SHIM_OFFLINE, kind = WEB_DOC_HTML_PAGE;
    memcpy(args, &entries, 2);
    memcpy(args + 4, &shim, 4);
    size_t off = tlv_put(args, 8, WEB_TLV_DOCUMENT_KIND, &kind, 4);
    char docpath[0x100] = {0}; /* the SDK's document path field: 0xC00 bytes there, this tree's paths fit in 0x100 */
    snprintf(docpath, sizeof docpath, "%s", rel);
    off = tlv_put(args, off, WEB_TLV_DOCUMENT_PATH, docpath, sizeof docpath);
    LibAppletCall call;
    memset(&call, 0, sizeof call);
    call.applet_id = LIBAPPLET_ID_OFFLINE_WEB;
    call.nin = 2;
    call.in[0] = (LibAppletIn){.data = common, .size = sizeof common};
    call.in[1] = (LibAppletIn){.data = args, .size = off};
    if (!applet_web_handler(&call) || !call.out || call.out_size < WEB_RETURN_SIZE) {
        libapplet_call_free(&call);
        return -1;
    }
    uint32_t reason;
    uint64_t n;
    memcpy(&reason, call.out, 4);
    memcpy(&n, call.out + 0x1008, 8);
    if (exit_reason) *exit_reason = reason;
    if (last_url && cap) {
        if (n >= cap) n = cap - 1;
        memcpy(last_url, call.out + 8, n);
        last_url[n] = 0;
    }
    libapplet_call_free(&call);
    return reason == WEB_EXIT_ERROR ? -1 : 0;
}

void web_file_url(const char *path, char *out, size_t cap) {
    size_t k = 0;
    const char *pre = "file://";
    for (const char *s = pre; *s && k + 1 < cap; s++) out[k++] = *s;
    for (const char *s = path; *s && k + 1 < cap; s++) {
        if (*s == ' ') {
            if (k + 3 >= cap) break;
            out[k++] = '%', out[k++] = '2', out[k++] = '0';
        } else out[k++] = *s;
    }
    out[k] = 0;
}

void web_return_encode(uint32_t exit_reason, const char *last_url, uint8_t out[WEB_RETURN_SIZE]) {
    memset(out, 0, WEB_RETURN_SIZE);
    memcpy(out, &exit_reason, 4);
    uint64_t n = 0;
    if (last_url) {
        n = strlen(last_url);
        if (n > WEB_URL_MAX - 1) n = WEB_URL_MAX - 1;
        memcpy(out + 8, last_url, n);
    }
    memcpy(out + 0x1008, &n, 8);
}

static char g_html_root[0x400];
const char *applet_web_html_root(void) { return g_html_root[0] ? g_html_root : getenv("NAPPLE_HTML_ROOT"); }
void applet_web_set_html_root(const char *dir) {
    if (dir) snprintf(g_html_root, sizeof g_html_root, "%s", dir);
    else g_html_root[0] = 0;
}

/* The WebArgs storage is the last pushed input after CommonArguments. */
int applet_web_handler(LibAppletCall *call) {
    const LibAppletIn *in = NULL;
    for (unsigned i = 1; i < call->nin; i++) /* in[0] is CommonArguments */
        if (call->in[i].data) in = &call->in[i];
    WebArgs *a = calloc(1, sizeof *a);
    if (!a) return 0;
    if (in) web_args_parse(in->data, in->size, a);
    char url[WEB_URL_MAX + 16] = "", last[WEB_URL_MAX] = "";
    uint32_t reason = WEB_EXIT_BACK_BUTTON;
    int offline = call->applet_id == LIBAPPLET_ID_OFFLINE_WEB || a->shim_kind == WEB_SHIM_OFFLINE;
    int ok = 1;
    if (!a->valid) ok = 0;
    else if (offline) {
        const char *root = g_html_root[0] ? g_html_root : getenv("NAPPLE_HTML_ROOT");
        char path[0x400];
        if (a->document_kind == WEB_DOC_HTML_PAGE || !a->has_document_kind) {
            if (a->has_document_path && root && web_offline_resolve(root, a->document_path, path, sizeof path) == 0) web_file_url(path, url, sizeof url);
            else ok = 0;
        } else ok = 0; /* legal information / system data pages live in firmware romfs: not available */
    } else if (a->has_url) snprintf(url, sizeof url, "%s", a->url);
    else ok = 0;
    if (ok) {
        int r = applet_web_script(getenv("NAPPLE_WEB_ANSWER"));
        if (r != -2) snprintf(last, sizeof last, "%s", url); /* scripted: leaves on the page it opened */
        else r = applet_ui_open_web(url, offline, last, sizeof last);
        if (r == -2) { /* no frontend: the user leaves at once, on the page it opened */
            snprintf(last, sizeof last, "%s", url);
            reason = WEB_EXIT_REQUESTED;
        } else if (r < 0) reason = WEB_EXIT_ERROR;
        else reason = (uint32_t)r;
    } else reason = WEB_EXIT_ERROR;
    uint8_t out[WEB_RETURN_SIZE];
    web_return_encode(reason, last, out);
    libapplet_call_set_out(call, out, sizeof out);
    call->result = 0;
    free(a);
    return 1;
}

int applet_web_register(void) {
    if (libapplet_register_handler(LIBAPPLET_ID_WEB, applet_web_handler)) return -1;
    return libapplet_register_handler(LIBAPPLET_ID_OFFLINE_WEB, applet_web_handler);
}
