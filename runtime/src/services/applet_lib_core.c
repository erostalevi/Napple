#include "applet_lib_core.h"
#include <stdio.h>
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
