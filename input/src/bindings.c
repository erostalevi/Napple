/* Binding tables: defaults, validation and JSON (input.json). */
#include "internal.h"
#include "tinyjson.h"
#include <stdio.h>
#include <string.h>

/* macOS virtual keycodes of the default layout */
enum {
    K_A = 0,
    K_S = 1,
    K_D = 2,
    K_F = 3,
    K_H = 4,
    K_G = 5,
    K_Z = 6,
    K_C = 8,
    K_Q = 12,
    K_W = 13,
    K_E = 14,
    K_T = 17,
    K_O = 31,
    K_I = 34,
    K_L = 37,
    K_K = 40,
    K_RETURN = 36,
    K_TAB = 48,
    K_SPACE = 49,
    K_SHIFT = 56,
    K_LEFT = 123,
    K_RIGHT = 124,
    K_DOWN = 125,
    K_UP = 126,
    K_ESC = 53,
    K_BACKSPACE = 51
};

void napple_bindings_default(napple_bindings *b) {
    memset(b, 0xff, sizeof *b); /* all keys NONE, all pad -1 */
    for (int i = 0; i < NAPPLE_BTN_COUNT_BASIC; i++) b->key[i][0] = b->key[i][1] = NAPPLE_KEY_NONE;
    b->key[NAPPLE_BTN_A][0] = K_L;
    b->key[NAPPLE_BTN_B][0] = K_K;
    b->key[NAPPLE_BTN_X][0] = K_O;
    b->key[NAPPLE_BTN_Y][0] = K_I;
    b->key[NAPPLE_BTN_STICK_L][0] = K_Z;
    b->key[NAPPLE_BTN_STICK_R][0] = K_C;
    b->key[NAPPLE_BTN_L][0] = K_Q;
    b->key[NAPPLE_BTN_R][0] = K_E;
    b->key[NAPPLE_BTN_ZL][0] = K_SHIFT;
    b->key[NAPPLE_BTN_ZR][0] = K_SPACE;
    b->key[NAPPLE_BTN_PLUS][0] = K_RETURN;
    b->key[NAPPLE_BTN_MINUS][0] = K_TAB;
    b->key[NAPPLE_BTN_LEFT][0] = K_F;
    b->key[NAPPLE_BTN_UP][0] = K_T;
    b->key[NAPPLE_BTN_RIGHT][0] = K_H;
    b->key[NAPPLE_BTN_DOWN][0] = K_G;
    const uint16_t sl[4] = {K_A, K_W, K_D, K_S}, sr[4] = {K_LEFT, K_UP, K_RIGHT, K_DOWN};
    memcpy(b->stick_l, sl, sizeof sl);
    memcpy(b->stick_r, sr, sizeof sr);
    for (int i = 0; i < NAPPLE_BTN_COUNT_BASIC; i++) b->pad[i][0] = b->pad[i][1] = -1;
    /* by position: Switch A is the east button, B south, X north, Y west */
    b->pad[NAPPLE_BTN_A][0] = NAPPLE_PAD_EAST;
    b->pad[NAPPLE_BTN_B][0] = NAPPLE_PAD_SOUTH;
    b->pad[NAPPLE_BTN_X][0] = NAPPLE_PAD_NORTH;
    b->pad[NAPPLE_BTN_Y][0] = NAPPLE_PAD_WEST;
    b->pad[NAPPLE_BTN_STICK_L][0] = NAPPLE_PAD_L3;
    b->pad[NAPPLE_BTN_STICK_R][0] = NAPPLE_PAD_R3;
    b->pad[NAPPLE_BTN_L][0] = NAPPLE_PAD_LB;
    b->pad[NAPPLE_BTN_R][0] = NAPPLE_PAD_RB;
    b->pad[NAPPLE_BTN_ZL][0] = NAPPLE_PAD_LT;
    b->pad[NAPPLE_BTN_ZR][0] = NAPPLE_PAD_RT;
    b->pad[NAPPLE_BTN_PLUS][0] = NAPPLE_PAD_MENU;
    b->pad[NAPPLE_BTN_MINUS][0] = NAPPLE_PAD_OPTIONS;
    b->pad[NAPPLE_BTN_LEFT][0] = NAPPLE_PAD_DLEFT;
    b->pad[NAPPLE_BTN_UP][0] = NAPPLE_PAD_DUP;
    b->pad[NAPPLE_BTN_RIGHT][0] = NAPPLE_PAD_DRIGHT;
    b->pad[NAPPLE_BTN_DOWN][0] = NAPPLE_PAD_DDOWN;
    b->stick_deadzone = 0.12f;
    b->mouse_touch = 1;
    b->face_layout = 0;
    b->stick_range = 1.0f;
    b->stick_curve = 1.0f;
    b->trigger_threshold = 0.5f;
    memset(b->invert, 0, sizeof b->invert);
    b->style = NAPPLE_STYLE_PREF_AUTO;
    b->operation_mode = 0;
}

/* 0 if the table is acceptable */
int napple_bindings_validate(const napple_bindings *b) {
    for (int i = 0; i < NAPPLE_BTN_COUNT_BASIC; i++)
        for (int k = 0; k < 2; k++) {
            if (b->key[i][k] != NAPPLE_KEY_NONE && b->key[i][k] > 0xff) return -1;
            if (b->pad[i][k] < -1 || b->pad[i][k] >= NAPPLE_PAD_COUNT) return -1;
        }
    for (int i = 0; i < 4; i++)
        if ((b->stick_l[i] != NAPPLE_KEY_NONE && b->stick_l[i] > 0xff) || (b->stick_r[i] != NAPPLE_KEY_NONE && b->stick_r[i] > 0xff)) return -1;
    if (!(b->stick_deadzone >= 0.f && b->stick_deadzone <= 0.9f)) return -1;
    if (b->face_layout < 0 || b->face_layout > 2) return -1;
    if (!(b->stick_range >= 0.5f && b->stick_range <= 1.0f) || !(b->stick_deadzone < b->stick_range)) return -1;
    if (!(b->stick_curve >= 0.5f && b->stick_curve <= 3.0f)) return -1;
    if (!(b->trigger_threshold >= 0.05f && b->trigger_threshold <= 0.95f)) return -1;
    for (int i = 0; i < 4; i++)
        if (b->invert[i] > 1) return -1;
    if (b->style < 0 || b->style >= NAPPLE_STYLE_PREF_COUNT || (b->operation_mode != 0 && b->operation_mode != 1)) return -1;
    return 0;
}

static void put(char *out, size_t cap, size_t *at, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void put(char *out, size_t cap, size_t *at, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*at < cap ? out + *at : NULL, *at < cap ? cap - *at : 0, fmt, ap);
    va_end(ap);
    if (n > 0) *at += (size_t)n;
}

size_t napple_bindings_to_json(const napple_bindings *b, char *out, size_t cap) {
    size_t at = 0;
    static const char *const face[] = {"auto", "position", "label"};
    static const char *const sty[] = {"auto", "pro", "joycon", "handheld", "gc", "palma", "nes"};
    put(out, cap, &at,
        "{\n  \"version\": 1,\n  \"deadzone\": %.3f,\n  \"stick_range\": %.3f,\n  \"stick_curve\": %.3f,\n  \"trigger_threshold\": %.3f,\n"
        "  \"invert\": [%s, %s, %s, %s],\n  \"style\": \"%s\",\n  \"operation_mode\": \"%s\",\n  \"mouse_touch\": %s,\n  \"face_layout\": \"%s\",\n  \"keys\": {\n",
        b->stick_deadzone, b->stick_range, b->stick_curve, b->trigger_threshold, b->invert[0] ? "true" : "false", b->invert[1] ? "true" : "false",
        b->invert[2] ? "true" : "false", b->invert[3] ? "true" : "false", sty[b->style >= 0 && b->style < NAPPLE_STYLE_PREF_COUNT ? b->style : 0],
        b->operation_mode ? "handheld" : "docked", b->mouse_touch ? "true" : "false", face[b->face_layout >= 0 && b->face_layout < 3 ? b->face_layout : 0]);
    for (int i = 0; i < NAPPLE_BTN_COUNT_BASIC; i++)
        put(out, cap, &at, "    \"%s\": [%d, %d]%s\n", napple_button_name(i), b->key[i][0] == NAPPLE_KEY_NONE ? -1 : b->key[i][0],
            b->key[i][1] == NAPPLE_KEY_NONE ? -1 : b->key[i][1], i + 1 < NAPPLE_BTN_COUNT_BASIC ? "," : "");
    put(out, cap, &at, "  },\n");
    const uint16_t *st[2] = {b->stick_l, b->stick_r};
    const char *const names[2] = {"stick_left", "stick_right"};
    for (int s = 0; s < 2; s++) {
        put(out, cap, &at, "  \"%s\": [", names[s]);
        for (int i = 0; i < 4; i++) put(out, cap, &at, "%d%s", st[s][i] == NAPPLE_KEY_NONE ? -1 : st[s][i], i < 3 ? ", " : "");
        put(out, cap, &at, "],\n");
    }
    put(out, cap, &at, "  \"pad\": {\n");
    for (int i = 0; i < NAPPLE_BTN_COUNT_BASIC; i++) {
        put(out, cap, &at, "    \"%s\": [", napple_button_name(i));
        int first = 1;
        for (int k = 0; k < 2; k++)
            if (b->pad[i][k] >= 0) {
                put(out, cap, &at, "%s\"%s\"", first ? "" : ", ", napple_pad_element_name(b->pad[i][k]));
                first = 0;
            }
        put(out, cap, &at, "]%s\n", i + 1 < NAPPLE_BTN_COUNT_BASIC ? "," : "");
    }
    put(out, cap, &at, "  }\n}\n");
    return at;
}

static int key_value(const tj_node *n) {
    if (!n || n->type != TJ_NUM) return -2;
    int v = (int)n->num;
    if (n->num != (double)v || v < -1 || v > 0xff) return -2;
    return v < 0 ? NAPPLE_KEY_NONE : v;
}

int napple_bindings_from_json(const char *json, size_t len, napple_bindings *out) {
    tj_node pool[256];
    if (!json || len > 32768) return -1;
    int root = tj_parse(json, len, pool, sizeof pool / sizeof *pool);
    if (root < 0 || pool[root].type != TJ_OBJ) return -1;
    const tj_node *r = &pool[root];
    if (tj_num(tj_get(pool, r, "version"), 1) != 1) return -2;
    napple_bindings b;
    napple_bindings_default(&b);
    const tj_node *dz = tj_get(pool, r, "deadzone");
    if (dz) {
        if (dz->type != TJ_NUM) return -1;
        b.stick_deadzone = (float)dz->num;
    }
    const tj_node *x;
    if ((x = tj_get(pool, r, "stick_range"))) {
        if (x->type != TJ_NUM) return -1;
        b.stick_range = (float)x->num;
    }
    if ((x = tj_get(pool, r, "stick_curve"))) {
        if (x->type != TJ_NUM) return -1;
        b.stick_curve = (float)x->num;
    }
    if ((x = tj_get(pool, r, "trigger_threshold"))) {
        if (x->type != TJ_NUM) return -1;
        b.trigger_threshold = (float)x->num;
    }
    if ((x = tj_get(pool, r, "invert"))) {
        if (x->type != TJ_ARR || tj_len(pool, x) != 4) return -1;
        for (size_t i = 0; i < 4; i++) {
            const tj_node *e = tj_at(pool, x, i);
            if (!e || e->type != TJ_BOOL) return -1;
            b.invert[i] = (uint8_t)tj_bool(e, 0);
        }
    }
    if ((x = tj_get(pool, r, "style"))) {
        char s[16];
        if (x->type != TJ_STR) return -1;
        tj_str(x, s, sizeof s);
        if (!strcmp(s, "auto")) b.style = 0;
        else if (!strcmp(s, "pro")) b.style = 1;
        else if (!strcmp(s, "joycon")) b.style = 2;
        else if (!strcmp(s, "handheld")) b.style = 3;
        else if (!strcmp(s, "gc")) b.style = NAPPLE_STYLE_PREF_GC;
        else if (!strcmp(s, "palma")) b.style = NAPPLE_STYLE_PREF_PALMA;
        else if (!strcmp(s, "nes")) b.style = NAPPLE_STYLE_PREF_NES;
        else return -1;
    }
    if ((x = tj_get(pool, r, "operation_mode"))) {
        char s[16];
        if (x->type != TJ_STR) return -1;
        tj_str(x, s, sizeof s);
        if (!strcmp(s, "docked")) b.operation_mode = 0;
        else if (!strcmp(s, "handheld")) b.operation_mode = 1;
        else return -1;
    }
    const tj_node *mt = tj_get(pool, r, "mouse_touch");
    if (mt) b.mouse_touch = tj_bool(mt, 1);
    const tj_node *fl = tj_get(pool, r, "face_layout");
    if (fl) {
        char s[16];
        tj_str(fl, s, sizeof s);
        if (!strcmp(s, "auto")) b.face_layout = 0;
        else if (!strcmp(s, "position")) b.face_layout = 1;
        else if (!strcmp(s, "label")) b.face_layout = 2;
        else return -1;
    }
    const tj_node *keys = tj_get(pool, r, "keys");
    if (keys) {
        if (keys->type != TJ_OBJ) return -1;
        for (int c = keys->child; c >= 0; c = pool[c].next) {
            char name[16];
            size_t kn = pool[c].key_n < 15 ? pool[c].key_n : 15;
            memcpy(name, pool[c].key, kn);
            name[kn] = 0;
            int btn = napple_button_from_name(name);
            if (btn < 0) continue; /* unknown buttons are ignored (forward compatible) */
            if (pool[c].type != TJ_ARR) return -1;
            for (int k = 0; k < 2; k++) {
                const tj_node *e = tj_at(pool, &pool[c], (size_t)k);
                if (!e) {
                    b.key[btn][k] = NAPPLE_KEY_NONE;
                    continue;
                }
                int v = key_value(e);
                if (v == -2) return -1;
                b.key[btn][k] = (uint16_t)v;
            }
        }
    }
    uint16_t *sticks[2] = {b.stick_l, b.stick_r};
    const char *const sn[2] = {"stick_left", "stick_right"};
    for (int s = 0; s < 2; s++) {
        const tj_node *a = tj_get(pool, r, sn[s]);
        if (!a) continue;
        if (a->type != TJ_ARR || tj_len(pool, a) != 4) return -1;
        for (int i = 0; i < 4; i++) {
            int v = key_value(tj_at(pool, a, (size_t)i));
            if (v == -2) return -1;
            sticks[s][i] = (uint16_t)v;
        }
    }
    const tj_node *pad = tj_get(pool, r, "pad");
    if (pad) {
        if (pad->type != TJ_OBJ) return -1;
        for (int c = pad->child; c >= 0; c = pool[c].next) {
            char name[16];
            size_t kn = pool[c].key_n < 15 ? pool[c].key_n : 15;
            memcpy(name, pool[c].key, kn);
            name[kn] = 0;
            int btn = napple_button_from_name(name);
            if (btn < 0) continue;
            if (pool[c].type != TJ_ARR || tj_len(pool, &pool[c]) > 2) return -1;
            b.pad[btn][0] = b.pad[btn][1] = -1;
            for (size_t k = 0; k < tj_len(pool, &pool[c]); k++) {
                const tj_node *e = tj_at(pool, &pool[c], k);
                char en[16];
                if (!e || e->type != TJ_STR) return -1;
                tj_str(e, en, sizeof en);
                int el = napple_pad_element_from_name(en);
                if (el < 0) return -1;
                b.pad[btn][k] = (int16_t)el;
            }
        }
    }
    if (napple_bindings_validate(&b)) return -1;
    *out = b;
    return 0;
}
