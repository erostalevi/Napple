/* Web (0x13) and offline web (0x17) library applets, portable. The guest pushes CommonArguments, then the WebArgs storage: a header { u16 entries;
 * u16 pad; u32 shim_kind } (8 bytes) followed by TLV entries { u16 type; u16 size; u32 pad } (8 bytes) + `size` data bytes. The applet answers
 * WebCommonReturnValue { u32 exit_reason; u32 pad; char last_url[0x1000]; u64 last_url_size } (0x1010). Layouts and tag numbers follow libnx's public
 * web.h as remembered and are [unverified] against a title. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "applet_lib_core.h"

enum { LIBAPPLET_ID_WEB = 0x13, LIBAPPLET_ID_OFFLINE_WEB = 0x17 };
enum { WEB_SHIM_SHOP = 1, WEB_SHIM_LOGIN = 2, WEB_SHIM_OFFLINE = 3, WEB_SHIM_SHARE = 4, WEB_SHIM_WEB = 5, WEB_SHIM_WIFI = 6, WEB_SHIM_LOBBY = 7 };
enum {
    WEB_TLV_URL = 0x1,
    WEB_TLV_CALLBACK_URL = 0x2,
    WEB_TLV_CALLBACKABLE_URL = 0x3,
    WEB_TLV_APPLICATION_ID = 0x4,
    WEB_TLV_DOCUMENT_PATH = 0x5,
    WEB_TLV_DOCUMENT_KIND = 0x6,
    WEB_TLV_SYSTEM_DATA_ID = 0x7,
    WEB_TLV_WHITELIST = 0x8,
    WEB_TLV_SCREENSHOT_ENABLED = 0xB
};
enum { WEB_DOC_HTML_PAGE = 1, WEB_DOC_LEGAL_INFO = 2, WEB_DOC_SYSTEM_DATA = 3 };
/* Why the applet closed (WebExitReason, [unverified numbering]). */
enum { WEB_EXIT_BACK_BUTTON = 0, WEB_EXIT_REQUESTED = 1, WEB_EXIT_LAST_URL = 2, WEB_EXIT_CALLBACK_URL = 3, WEB_EXIT_WINDOW_CLOSED = 4, WEB_EXIT_ERROR = 7 };
#define WEB_URL_MAX 0x1000u
#define WEB_RETURN_SIZE 0x1010u

typedef struct {
    int valid;
    uint32_t shim_kind;
    unsigned entries;
    char url[WEB_URL_MAX], callback_url[WEB_URL_MAX], document_path[0x100];
    uint32_t document_kind;
    uint64_t application_id;
    int has_url, has_callback, has_document_path, has_document_kind, screenshot_enabled;
} WebArgs;
/* Parses the storage; a truncated entry stops the walk (valid stays 1 when the header was readable). */
void web_args_parse(const uint8_t *data, size_t size, WebArgs *a);

/* Offline document path -> file under `html_root` (the extracted HtmlDocument / manual romfs on the host). Strips "offline:/" / "offline:" and
 * leading slashes, rejects "..", backslashes and NULs, a directory or empty path becomes <dir>/index.html. Writes "<html_root>/<rel>" to out.
 * Returns 0 and fills out when the file exists, -1 for a bad path, -2 when no such file, -3 when out is too small. */
int web_offline_resolve(const char *html_root, const char *doc_path, char *out, size_t cap);
/* "file://" URL for a resolved host path (no escaping beyond spaces -> %20). */
void web_file_url(const char *path, char *out, size_t cap);

/* WebCommonReturnValue encoding. */
void web_return_encode(uint32_t exit_reason, const char *last_url, uint8_t out[WEB_RETURN_SIZE]);

/* Root of the title's offline HTML documents on the host (NAPPLE_HTML_ROOT in the registered handler when unset). NULL = none. */
void applet_web_set_html_root(const char *dir);
const char *applet_web_html_root(void); /* the root in effect (NAPPLE_HTML_ROOT when none was set), NULL = none */
/* Registers the handlers for 0x13 and 0x17 through libapplet_register_handler. 0 ok. */
int applet_web_register(void);
/* The handler itself (also used directly by tests). Answers with WebCommonReturnValue. */
int applet_web_handler(LibAppletCall *call);
