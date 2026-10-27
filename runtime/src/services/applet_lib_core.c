#include "applet_lib_core.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

void applet_error_parse(const uint8_t *h, size_t hn, const uint8_t *arg, size_t an, AppletErrorInfo *e) {
    memset(e, 0, sizeof *e);
    if (h && hn >= 2) {
        e->valid = 1;
        e->type = h[0];
        e->jump_flag = h[1];
    }
    if (arg && an >= 4) {
        memcpy(&e->first_word, arg, 4);
        e->has_word = 1;
    }
    if (arg && an >= 8) {
        memcpy(&e->second_word, arg + 4, 4);
        e->has_word2 = 1;
    }
}
void applet_error_describe(const AppletErrorInfo *e, char *buf, size_t cap) {
    if (!e->valid) {
        snprintf(buf, cap, "no header");
        return;
    }
    int n = snprintf(buf, cap, "type %u, jump-to-settings %s", e->type, e->jump_flag ? "requested (not done)" : "no");
    if (e->has_word && n > 0 && (size_t)n < cap) snprintf(buf + n, cap - (size_t)n, ", first argument word %#x", e->first_word);
}

void applet_error_code_text(const AppletErrorInfo *e, char *buf, size_t cap) {
    if (!cap) return;
    buf[0] = 0;
    if (!e->valid || !e->has_word) return;
    if (e->type == 1 && e->has_word2) snprintf(buf, cap, "%04u-%04u", e->first_word % 10000u, e->second_word % 10000u);
    else snprintf(buf, cap, "2%03u-%04u", e->first_word & 0x1ffu, (e->first_word >> 9) & 0x1fffu);
}

void applet_swkbd_parse(const uint8_t *k, size_t n, AppletSwkbdConfig *c) {
    memset(c, 0, sizeof *c);
    if (!k || n < 0x3B8) return;
    memcpy(&c->type, k, 4);
    memcpy(&c->max_len, k + 0x3AC, 4);
    memcpy(&c->min_len, k + 0x3B0, 4);
    memcpy(&c->password, k + 0x3B4, 4);
    c->password = c->password != 0;
    c->numeric = c->type == 1;
    if (c->max_len > APPLET_SWKBD_MAX_LEN) c->max_len = APPLET_SWKBD_MAX_LEN;
}
int applet_swkbd_fit(const AppletSwkbdConfig *c, const char *text, char *out, size_t cap) {
    size_t o = 0;
    unsigned chars = 0;
    if (!cap) return 0;
    for (const char *p = text ? text : ""; *p;) {
        size_t l = 1;
        unsigned char b = (unsigned char)*p;
        if (b >= 0xf0) l = 4;
        else if (b >= 0xe0) l = 3;
        else if (b >= 0xc0) l = 2;
        for (size_t i = 1; i < l; i++)
            if (!p[i]) {
                l = i;
                break;
            }
        if (c->max_len && chars >= c->max_len) break;
        if (o + l + 1 > cap) break;
        memcpy(out + o, p, l);
        o += l;
        p += l;
        chars++;
    }
    out[o] = 0;
    return chars >= c->min_len;
}

const uint8_t *applet_player_select_pick(const uint8_t (*uids)[16], unsigned count, int index, int cancel) {
    if (cancel || !uids || !count) return NULL;
    return uids[(index >= 0 && (unsigned)index < count) ? (unsigned)index : 0];
}
void applet_player_select_answer(const uint8_t user_id[16], int cancel, uint8_t out[APPLET_PLAYER_SELECT_OUT_SIZE]) {
    memset(out, 0, APPLET_PLAYER_SELECT_OUT_SIZE);
    uint64_t r = cancel ? APPLET_PLAYER_SELECT_CANCELLED : APPLET_PLAYER_SELECT_OK;
    memcpy(out, &r, 8);
    if (!cancel && user_id) memcpy(out + 8, user_id, 16);
}

void applet_controller_answer(const uint8_t *arg, size_t an, int connected, AppletControllerAnswer *a) {
    int8_t lo = 0, hi = 0;
    int single = 0;
    if (arg && an >= 6) {
        lo = (int8_t)arg[0];
        hi = (int8_t)arg[1];
        single = arg[5] != 0;
    }
    a->min = lo > 0 ? (uint32_t)lo : 1;
    a->max = hi > 0 ? (uint32_t)hi : 4;
    if (a->min > a->max) a->min = a->max;
    a->connected = connected < 1 ? 1 : connected;
    a->short_of_min = (uint32_t)a->connected < a->min;
    a->players = a->connected;
    if ((uint32_t)a->players < a->min) a->players = (int32_t)a->min;
    if ((uint32_t)a->players > a->max) a->players = (int32_t)a->max;
    a->single_mode = single;
    a->selected_id = single ? 0 : 0xffffffffu;
}
void applet_controller_encode(const AppletControllerAnswer *a, uint8_t out[APPLET_CONTROLLER_OUT_SIZE]) {
    memset(out, 0, APPLET_CONTROLLER_OUT_SIZE);
    memcpy(out, &a->players, 4);
    memcpy(out + 4, &a->selected_id, 4);
}
void applet_controller_encode_result(const AppletControllerAnswer *a, uint32_t result, uint8_t out[APPLET_CONTROLLER_OUT_SIZE]) {
    applet_controller_encode(a, out);
    memcpy(out + 8, &result, 4);
}

#define LIBAPPLET_MAX_HANDLERS 32
static struct {
    uint32_t id;
    LibAppletHandler fn;
} g_handlers[LIBAPPLET_MAX_HANDLERS];
static unsigned g_nhandlers;

int libapplet_register_handler(uint32_t id, LibAppletHandler handler) {
    for (unsigned i = 0; i < g_nhandlers; i++)
        if (g_handlers[i].id == id) {
            g_handlers[i].fn = handler;
            return 0;
        }
    if (!handler) return 0;
    if (g_nhandlers >= LIBAPPLET_MAX_HANDLERS) return -1;
    g_handlers[g_nhandlers].id = id;
    g_handlers[g_nhandlers++].fn = handler;
    return 0;
}
LibAppletHandler libapplet_find_handler(uint32_t id) {
    for (unsigned i = 0; i < g_nhandlers; i++)
        if (g_handlers[i].id == id) return g_handlers[i].fn;
    return NULL;
}
void libapplet_clear_handlers(void) { g_nhandlers = 0; }
int libapplet_dispatch(LibAppletCall *call) {
    LibAppletHandler h = libapplet_find_handler(call->applet_id);
    return h ? h(call) : 0;
}
void libapplet_call_set_out(LibAppletCall *call, const void *data, size_t size) {
    free(call->out);
    call->out = calloc(1, size ? size : 1);
    call->out_size = size;
    if (data && size) memcpy(call->out, data, size);
}
int libapplet_call_push_interactive_out(LibAppletCall *call, const void *data, size_t size) {
    if (call->ninteractive_out >= LIBAPPLET_MAX_IN) return -1;
    uint8_t *p = calloc(1, size ? size : 1);
    if (data && size) memcpy(p, data, size);
    call->interactive_out[call->ninteractive_out] = p;
    call->interactive_out_size[call->ninteractive_out++] = size;
    return 0;
}
void libapplet_call_free(LibAppletCall *call) {
    free(call->out);
    call->out = NULL;
    for (unsigned i = 0; i < call->ninteractive_out; i++) free(call->interactive_out[i]);
    call->ninteractive_out = 0;
}

static const LibAppletProgram g_programs[] = {
    {0x0a, "0100000000001001", "auth", 0},         {0x0b, "0100000000001002", "cabinet", 0},    {0x0c, "0100000000001003", "controller", 0},
    {0x0d, "0100000000001004", "dataErase", 0},    {0x0e, "0100000000001005", "error", 0},      {0x0f, "0100000000001006", "netConnect", 0},
    {0x10, "0100000000001007", "playerSelect", 0}, {0x11, "0100000000001008", "swkbd", 0},      {0x12, "0100000000001009", "miiEdit", 1},
    {0x13, "010000000000100a", "web", 0},          {0x14, "010000000000100b", "shop", 0},       {0x15, "010000000000100d", "photoViewer", 0},
    {0x16, "010000000000100e", "set", 0},          {0x17, "010000000000100f", "offlineWeb", 0}, {0x18, "0100000000001010", "loginShare", 0},
    {0x19, "0100000000001011", "wifiWebAuth", 0},  {0x1a, "0100000000001013", "myPage", 0},
};
const LibAppletProgram *libapplet_program_for(uint32_t id) {
    for (unsigned i = 0; i < sizeof g_programs / sizeof *g_programs; i++)
        if (g_programs[i].applet_id == id) return &g_programs[i];
    return NULL;
}
static int ids_match(const LibAppletProgram *p, const char *ids) {
    if (!ids) return 0;
    for (const char *t = ids; *t;) {
        while (*t == ',' || *t == ' ') t++;
        const char *e = t;
        while (*e && *e != ',' && *e != ' ') e++;
        size_t n = (size_t)(e - t);
        if (n) {
            char tok[32];
            snprintf(tok, sizeof tok, "%.*s", (int)(n < 31 ? n : 31), t);
            char *end;
            unsigned long v = strtoul(tok, &end, 0);
            if (!*end && v == p->applet_id) return 1;
            if (!strcasecmp(tok, p->name)) return 1;
        }
        t = e;
    }
    return 0;
}
int libapplet_firmware_enabled(const LibAppletProgram *p, const char *master, const char *ids) {
    if (!p) return 0;
    if (master && !strcmp(master, "0")) return 0;
    if (master && !strcmp(master, "all")) return 1;
    return p->default_firmware || ids_match(p, ids);
}
int libapplet_mode_is_inline(uint32_t mode) { return mode != 0 && mode != 4; }

void applet_swkbd_inline_init(AppletSwkbdInline *k, const AppletSwkbdConfig *cfg) {
    memset(k, 0, sizeof *k);
    if (cfg) k->cfg = *cfg;
}
int applet_swkbd_inline_reply(LibAppletCall *call, uint32_t type, const char *text) {
    uint8_t buf[APPLET_SWKBD_INLINE_REPLY_SIZE];
    memset(buf, 0, sizeof buf);
    uint32_t n = 0;
    if (text) {
        n = (uint32_t)strlen(text);
        if (n > sizeof buf - 12) n = (uint32_t)sizeof buf - 12;
        memcpy(buf + 8, text, n);
    }
    memcpy(buf, &type, 4);
    memcpy(buf + 4, &n, 4);
    return libapplet_call_push_interactive_out(call, buf, 8 + n + 1);
}
int applet_swkbd_inline_request(AppletSwkbdInline *k, const uint8_t *req, size_t size, const char *text, LibAppletCall *call) {
    uint32_t cmd = 0xffffffffu;
    if (req && size >= 4) memcpy(&cmd, req, 4);
    k->requests++;
    switch (cmd) {
        case APPLET_SWKBD_CMD_FINALIZE:
            k->finalized = 1;
            k->shown = 0;
            return 1;
        case APPLET_SWKBD_CMD_CALC: {
            if (k->finalized) return 1;
            k->shown = 1;
            char fit[APPLET_SWKBD_MAX_LEN * 4 + 4];
            if (text && applet_swkbd_fit(&k->cfg, text, fit, sizeof fit)) {
                applet_swkbd_inline_reply(call, APPLET_SWKBD_REPLY_CHANGED_STRING_UTF8, fit);
                applet_swkbd_inline_reply(call, APPLET_SWKBD_REPLY_DECIDED_ENTER_UTF8, fit);
            } else applet_swkbd_inline_reply(call, APPLET_SWKBD_REPLY_DECIDED_CANCEL, NULL);
            k->shown = 0;
            return 1;
        }
        case APPLET_SWKBD_CMD_SET_USER_WORD:
        case APPLET_SWKBD_CMD_SET_CUSTOMIZE_DIC:
        case APPLET_SWKBD_CMD_SET_CUSTOMIZED_DICTIONARIES: return 1; /* dictionaries only change suggestions, which the host text bar does not show */
        case APPLET_SWKBD_CMD_UNSET_CUSTOMIZED_DICTIONARIES: applet_swkbd_inline_reply(call, APPLET_SWKBD_REPLY_UNSET_CUSTOMIZED, NULL); return 1;
        default: applet_swkbd_inline_reply(call, APPLET_SWKBD_REPLY_DEFAULT, NULL); return 0;
    }
}

static AppletErrorHook g_error_hook;
void applet_set_error_hook(AppletErrorHook hook) { g_error_hook = hook; }
int applet_error_notify(const AppletErrorInfo *e, const char *code_text, const char *description) {
    if (!g_error_hook) return 0;
    g_error_hook(e, code_text, description);
    return 1;
}

static AppletProfileLister g_profile_lister;
void applet_set_profile_lister(AppletProfileLister lister) { g_profile_lister = lister; }
unsigned applet_profiles(uint8_t (*uids)[16], unsigned max, const uint8_t fallback[16]) {
    unsigned n = g_profile_lister && max ? g_profile_lister(uids, max) : 0;
    if (n > max) n = max;
    if (!n && fallback && max) {
        memcpy(uids[0], fallback, 16);
        n = 1;
    }
    return n;
}
int applet_player_select_spec(const char *spec, int *index) {
    *index = 0;
    if (!spec || !*spec) return 0;
    if (!strcmp(spec, "cancel")) return 1;
    *index = atoi(spec);
    return 0;
}

void applet_controller_apply_policy(const char *spec, AppletControllerAnswer *a, uint32_t *result) {
    *result = APPLET_CONTROLLER_OK;
    if (!spec) return;
    if (!strcmp(spec, "cancel") || (!strcmp(spec, "strict") && a->short_of_min)) {
        *result = APPLET_CONTROLLER_CANCELLED;
        a->players = 0;
        a->selected_id = 0xffffffffu;
    }
}
