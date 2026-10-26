/* Input backend: host keyboard / mouse / GameController state -> Switch controller state in HID shared memory.

   Policy (documented in docs/audio-input-frontend-interface.md):
     - Player 1 (npad id 0) is always present: the keyboard (and the first controller) drive it.
     - Players 2..N follow the 2nd..Nth connected controller, limited by SetSupportedNpadIdType.
     - The reported style is the first of FullKey, JoyDual, Handheld, JoyLeft, JoyRight that the game supports. A game
       that supports only Handheld gets its state on the handheld npad (id 0x20, slot 9) as well as on player 1.
     - The left mouse button is a one-finger touch screen (1280x720 space) when bindings.mouse_touch is set.
   Everything is sampled by one thread (default 120 Hz) that writes the shared memory with the pure writers in
   hid_shm.c; napple_input_update can also be called by the embedder. */
#include "internal.h"
#include "tinyjson.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STICK_MAX 32767.f
#define NSLOTS 10

void napple_input_log(const char *fmt, ...) {
    char line[400]; int n = snprintf(line, sizeof line, "[napple] input: ");
    va_list ap; va_start(ap, fmt); vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap); va_end(ap);
    fprintf(stderr, "%s\n", line);
}

struct napple_input {
    napple_input_config cfg;
    char dir[1024], path[1100];
    pthread_mutex_t lock;
    napple_hid_layout layout;
    napple_bindings bind;
    /* host events */
    uint8_t keys[256];
    float mouse_nx, mouse_ny, scroll_dx, scroll_dy; int mouse_buttons, mouse_have_pos; int32_t last_mx, last_my;
    int focused, blocked;
    uint64_t shake_start_ns; int shake_req;   /* napple_input_shake: a side-to-side shake of player 1's six-axis sensor (0 = none) */
    /* on-screen controller (touch devices): buttons NAPPLE_BTN_MASK bits and analog sticks in [-1,1], y up; merged into player 1 */
    uint64_t virt_buttons; float virt_lx, virt_ly, virt_rx, virt_ry;
    /* guest configuration */
    uint32_t supported_styles, assign_mode; int player_count, sixaxis_enabled;
    /* sampled state */
    ng_pad pads[NG_MAX_PADS]; int npads;
    napple_motion motion[NG_MAX_PADS]; uint64_t motion_seen[NG_MAX_PADS];
    uint32_t prev_elements[NG_MAX_PADS];
    napple_npad_state pad_state[NSLOTS]; int slot_style[NSLOTS];
    napple_input_snapshot snap;
    int touch_down; uint8_t prev_keyboard[32]; uint64_t prev_modifiers; napple_mouse_state prev_mouse;
    uint64_t last_ns;
    /* capture */
    napple_capture_fn capture; void *capture_user;
    /* hot keys + vibration bookkeeping */
    napple_hotkey_fn hotkey; void *hotkey_user; uint32_t prev_hot[NG_MAX_PADS];
    napple_vibration vib[NSLOTS][2]; int vib_set[NSLOTS][2];
    /* thread */
    pthread_t thread; int thread_on; _Atomic int running;
    uint8_t *own_shm;
};

/* ------------------------------------------------------------------ files */
static void sanitize_id(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (; in && *in && n + 1 < cap; in++) if ((*in >= '0' && *in <= '9') || (*in >= 'a' && *in <= 'f') || (*in >= 'A' && *in <= 'F')) out[n++] = *in;
    out[n] = 0;
    if (!n) strlcpy(out, "default", cap);
}
static int mkdir_p(const char *path) {
    char tmp[1100]; strlcpy(tmp, path, sizeof tmp);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; if (mkdir(tmp, 0755) && errno != EEXIST) return -1; *p = '/'; }
    return mkdir(tmp, 0755) && errno != EEXIST ? -1 : 0;
}
static void load_bindings(napple_input *in) {
    napple_bindings_default(&in->bind);
    FILE *f = fopen(in->path, "rb"); if (!f) return;
    char buf[32769]; size_t n = fread(buf, 1, sizeof buf - 1, f); int whole = feof(f); fclose(f);
    napple_bindings b;
    if (!whole || napple_bindings_from_json(buf, n, &b)) { napple_input_log("%s is invalid: using default bindings (file kept)", in->path); return; }
    in->bind = b; napple_input_log("bindings loaded from %s", in->path);
}
static int save_bindings(napple_input *in) {
    char json[16384]; size_t n = napple_bindings_to_json(&in->bind, json, sizeof json);
    if (n >= sizeof json) return -1;
    if (mkdir_p(in->dir)) return -1;
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.tmp", in->path);
    FILE *f = fopen(tmp, "wb"); if (!f) return -1;
    int ok = fwrite(json, 1, n, f) == n; ok &= fclose(f) == 0;
    if (!ok || rename(tmp, in->path)) { unlink(tmp); return -1; }
    return 0;
}

/* ------------------------------------------------------------------ lifecycle */
int napple_input_create(const napple_input_config *c, napple_input **out) {
    if (!c || !out) return -1;
    napple_input *in = calloc(1, sizeof *in); if (!in) return -1;
    in->cfg = *c; if (!in->cfg.update_hz) in->cfg.update_hz = 120;
    if (in->cfg.update_hz < 30 || in->cfg.update_hz > 1000) in->cfg.update_hz = 120;
    if (!in->cfg.screen_w) in->cfg.screen_w = 1280;
    if (!in->cfg.screen_h) in->cfg.screen_h = 720;
    pthread_mutex_init(&in->lock, NULL);
    if (c->config_dir && *c->config_dir) strlcpy(in->dir, c->config_dir, sizeof in->dir);
    else {
        char id[32]; sanitize_id(c->title_id, id, sizeof id);
        const char *home = getenv("HOME"); snprintf(in->dir, sizeof in->dir, "%s/Library/Application Support/Napple/%s", home && *home ? home : "/tmp", id);
    }
    snprintf(in->path, sizeof in->path, "%s/input.json", in->dir);
    napple_hid_layout_for(c->layout_profile, &in->layout);
    if (!in->cfg.shm) { in->own_shm = in->cfg.shm = calloc(1, NAPPLE_HID_SHM_SIZE); if (!in->own_shm) { free(in); return -1; } }
    napple_hid_init(in->cfg.shm, &in->layout);
    load_bindings(in);
    in->focused = 1; in->player_count = 1; in->sixaxis_enabled = 1;
    in->supported_styles = NAPPLE_STYLE_FULL_KEY | NAPPLE_STYLE_HANDHELD | NAPPLE_STYLE_JOY_DUAL;
    for (int i = 0; i < NG_MAX_PADS; i++) napple_motion_reset(&in->motion[i]);
    if (c->use_game_controller && !getenv("NAPPLE_INPUT_NO_GC")) ng_start();
    *out = in;
    return 0;
}

void napple_input_destroy(napple_input *in) {
    if (!in) return;
    napple_input_stop(in);
    if (in->cfg.use_game_controller) ng_stop();
    pthread_mutex_destroy(&in->lock);
    free(in->own_shm); free(in);
}

static uint64_t clock_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
#define SHAKE_HZ 5.0
#define SHAKE_CYCLES 3.0

static void *input_thread(void *p) {
    napple_input *in = p;
    const uint64_t period = 1000000000ull / (uint64_t)in->cfg.update_hz;
    uint64_t next = clock_ns();
    while (atomic_load(&in->running)) {
        napple_input_update(in, clock_ns());
        next += period;
        uint64_t now = clock_ns();
        if (next > now) { struct timespec d = {(time_t)((next - now) / 1000000000ull), (long)((next - now) % 1000000000ull)}; nanosleep(&d, NULL); }
        else if (now - next > 100000000ull) next = now;
    }
    return NULL;
}

int napple_input_start(napple_input *in) {
    if (!in || in->thread_on) return in ? 0 : -1;
    atomic_store(&in->running, 1);
    if (pthread_create(&in->thread, NULL, input_thread, in)) { atomic_store(&in->running, 0); return -1; }
    in->thread_on = 1; return 0;
}

void napple_input_stop(napple_input *in) {
    if (!in || !in->thread_on) return;
    atomic_store(&in->running, 0); pthread_join(in->thread, NULL); in->thread_on = 0;
    const char *dump = getenv("NAPPLE_HID_DUMP");
    if (dump && *dump) { FILE *f = fopen(dump, "wb"); if (f) { fwrite(in->cfg.shm, 1, NAPPLE_HID_SHM_SIZE, f); fclose(f); napple_input_log("HID shared memory dumped to %s", dump); } }
    ng_rumble_quiet();
}

/* ------------------------------------------------------------------ guest configuration */
void napple_input_set_supported_styles(napple_input *in, uint32_t s) { pthread_mutex_lock(&in->lock); in->supported_styles = s; pthread_mutex_unlock(&in->lock); }
void napple_input_set_assignment_mode(napple_input *in, uint32_t m) { pthread_mutex_lock(&in->lock); in->assign_mode = m ? 1 : 0; pthread_mutex_unlock(&in->lock); }
void napple_input_set_player_count(napple_input *in, int n) { pthread_mutex_lock(&in->lock); in->player_count = n < 1 ? 1 : n > 8 ? 8 : n; pthread_mutex_unlock(&in->lock); }
void napple_input_shake(napple_input *in) { pthread_mutex_lock(&in->lock); in->shake_req = 1; pthread_mutex_unlock(&in->lock); }
void napple_input_set_sixaxis_enabled(napple_input *in, int e) { pthread_mutex_lock(&in->lock); in->sixaxis_enabled = e != 0; pthread_mutex_unlock(&in->lock); ng_set_motion(e); }
uint32_t napple_input_active_style(const napple_input *in, int slot) { return slot >= 0 && slot < NSLOTS ? (uint32_t)in->slot_style[slot] : 0; }
void napple_input_set_vibration(napple_input *in, uint32_t npad_id, int side, const napple_vibration *v) {
    int slot = napple_npad_slot_from_id(npad_id);
    if (slot < 0 || !v || in->blocked) return;
    if (side < 0 || side > 1) return;
    int pad = slot == 9 ? 0 : slot;   /* handheld rumble goes to the first controller */
    napple_vibration c = *v;          /* clamp: the guest may send anything */
    c.amp_low = fminf(fmaxf(isfinite(c.amp_low) ? c.amp_low : 0, 0), 1); c.amp_high = fminf(fmaxf(isfinite(c.amp_high) ? c.amp_high : 0, 0), 1);
    c.freq_low = isfinite(c.freq_low) ? fminf(fmaxf(c.freq_low, 40), 1250) : 160; c.freq_high = isfinite(c.freq_high) ? fminf(fmaxf(c.freq_high, 40), 1250) : 320;
    pthread_mutex_lock(&in->lock); in->vib[slot][side] = c; in->vib_set[slot][side] = 1; pthread_mutex_unlock(&in->lock);
    if (pad < in->npads || pad == 0) ng_rumble(pad, side, &c);
}

int napple_input_get_vibration(napple_input *in, int slot, int side, napple_vibration *out) {
    if (!in || slot < 0 || slot >= NSLOTS || side < 0 || side > 1) return 0;
    pthread_mutex_lock(&in->lock); int ok = in->vib_set[slot][side]; if (ok && out) *out = in->vib[slot][side]; pthread_mutex_unlock(&in->lock);
    return ok;
}
void napple_input_set_hotkey_callback(napple_input *in, napple_hotkey_fn cb, void *user) { pthread_mutex_lock(&in->lock); in->hotkey = cb; in->hotkey_user = user; pthread_mutex_unlock(&in->lock); }
int napple_input_operation_mode(napple_input *in) { pthread_mutex_lock(&in->lock); int m = in->bind.operation_mode; pthread_mutex_unlock(&in->lock); return m; }

/* ------------------------------------------------------------------ host events */
void napple_input_capture_next(napple_input *in, napple_capture_fn cb, void *user) {
    pthread_mutex_lock(&in->lock); in->capture = cb; in->capture_user = user; pthread_mutex_unlock(&in->lock);
}
int napple_input_capture_pending(napple_input *in) { pthread_mutex_lock(&in->lock); int p = in->capture != NULL; pthread_mutex_unlock(&in->lock); return p; }
void napple_input_key(napple_input *in, uint16_t code, int down) {
    if (!in || code > 0xff) return;
    napple_capture_fn cb = NULL; void *user = NULL;
    pthread_mutex_lock(&in->lock);
    if (down && in->capture && !in->keys[code]) { cb = in->capture; user = in->capture_user; in->capture = NULL; }
    else if (!in->blocked) in->keys[code] = down ? 1 : 0;
    if (!down) in->keys[code] = 0;
    pthread_mutex_unlock(&in->lock);
    if (cb) cb(user, 1, code);
}
void napple_input_keys(napple_input *in, const uint16_t *codes, int n, int down) {
    if (!in || !codes) return;
    pthread_mutex_lock(&in->lock);
    for (int i = 0; i < n; i++) if (codes[i] <= 0xff) { if (down) { if (!in->blocked) in->keys[codes[i]] = 1; } else in->keys[codes[i]] = 0; }
    pthread_mutex_unlock(&in->lock);
}
void napple_input_mouse_position(napple_input *in, float nx, float ny) {
    pthread_mutex_lock(&in->lock);
    in->mouse_nx = nx < 0 ? 0 : nx > 1 ? 1 : nx; in->mouse_ny = ny < 0 ? 0 : ny > 1 ? 1 : ny; in->mouse_have_pos = 1;
    pthread_mutex_unlock(&in->lock);
}
void napple_input_mouse_button(napple_input *in, int b, int down) {
    if (b < 0 || b > 4) return;
    pthread_mutex_lock(&in->lock);
    if (!in->blocked || !down) { if (down) in->mouse_buttons |= 1 << b; else in->mouse_buttons &= ~(1 << b); }
    pthread_mutex_unlock(&in->lock);
}
void napple_input_mouse_scroll(napple_input *in, float dx, float dy) { pthread_mutex_lock(&in->lock); in->scroll_dx += dx; in->scroll_dy += dy; pthread_mutex_unlock(&in->lock); }
void napple_input_release_all(napple_input *in) {
    pthread_mutex_lock(&in->lock); memset(in->keys, 0, sizeof in->keys); in->mouse_buttons = 0; pthread_mutex_unlock(&in->lock);
    ng_rumble_quiet();
}
void napple_input_virtual_pad(napple_input *in, uint64_t buttons, float lx, float ly, float rx, float ry) {
    pthread_mutex_lock(&in->lock);
    if (getenv("NAPPLE_INPUT_LOG") && (buttons || in->virt_buttons)) napple_input_log("virtual pad buttons=%#llx focused=%d blocked=%d", (unsigned long long)buttons, in->focused, in->blocked);
    if (in->blocked || !in->focused) buttons = 0, lx = ly = rx = ry = 0;
    in->virt_buttons = buttons & (NAPPLE_BTN_MASK(NAPPLE_BTN_COUNT_BASIC) - 1);
    float *dst[4] = {&in->virt_lx, &in->virt_ly, &in->virt_rx, &in->virt_ry}; float src[4] = {lx, ly, rx, ry};
    for (int i = 0; i < 4; i++) *dst[i] = src[i] > 1 ? 1 : src[i] < -1 ? -1 : src[i];
    pthread_mutex_unlock(&in->lock);
}
void napple_input_focus(napple_input *in, int focused) {
    pthread_mutex_lock(&in->lock); in->focused = focused != 0; if (!focused) { memset(in->keys, 0, sizeof in->keys); in->mouse_buttons = 0; in->virt_buttons = 0; in->virt_lx = in->virt_ly = in->virt_rx = in->virt_ry = 0; } pthread_mutex_unlock(&in->lock);
    if (!focused) ng_rumble_quiet();
}
void napple_input_set_blocked(napple_input *in, int blocked) {
    pthread_mutex_lock(&in->lock); in->blocked = blocked != 0; if (blocked) { memset(in->keys, 0, sizeof in->keys); in->mouse_buttons = 0; in->virt_buttons = 0; in->virt_lx = in->virt_ly = in->virt_rx = in->virt_ry = 0; } pthread_mutex_unlock(&in->lock);
    if (blocked) ng_rumble_quiet();
}

/* ------------------------------------------------------------------ bindings api */
int napple_input_get_bindings(napple_input *in, napple_bindings *out) { pthread_mutex_lock(&in->lock); *out = in->bind; pthread_mutex_unlock(&in->lock); return 0; }
int napple_input_set_bindings(napple_input *in, const napple_bindings *b) {
    if (napple_bindings_validate(b)) return -1;
    pthread_mutex_lock(&in->lock); in->bind = *b; int rc = save_bindings(in); pthread_mutex_unlock(&in->lock);
    if (rc) napple_input_log("could not save %s: %s", in->path, strerror(errno));
    return rc;
}
int napple_input_reset_bindings(napple_input *in) { napple_bindings d; napple_bindings_default(&d); return napple_input_set_bindings(in, &d); }

/* ------------------------------------------------------------------ mapping (pure helpers, tested through update) */
static int held(const napple_input *in, uint16_t key) { return key != NAPPLE_KEY_NONE && key < 256 && in->keys[key]; }

/* radial dead zone, outer range and response curve: returns the stick in [-1,1] */
static void radial(float x, float y, float dz, float range, float curve, float *ox, float *oy) {
    float m = sqrtf(x * x + y * y);
    if (!(m > dz)) { *ox = *oy = 0; return; }
    float s = (m - dz) / (range - dz); if (s > 1) s = 1;
    if (curve != 1.0f) s = powf(s, curve);
    *ox = x / m * s; *oy = y / m * s;
}
static napple_stick to_stick(float x, float y) {
    float m = sqrtf(x * x + y * y); if (m > 1) { x /= m; y /= m; }
    return (napple_stick){(int32_t)lroundf(x * STICK_MAX), (int32_t)lroundf(y * STICK_MAX)};
}
static uint32_t swap_face(uint32_t e) {   /* south<->east, west<->north */
    uint32_t keep = e & ~((1u << NAPPLE_PAD_SOUTH) | (1u << NAPPLE_PAD_EAST) | (1u << NAPPLE_PAD_WEST) | (1u << NAPPLE_PAD_NORTH));
    if (e & (1u << NAPPLE_PAD_SOUTH)) keep |= 1u << NAPPLE_PAD_EAST;
    if (e & (1u << NAPPLE_PAD_EAST)) keep |= 1u << NAPPLE_PAD_SOUTH;
    if (e & (1u << NAPPLE_PAD_WEST)) keep |= 1u << NAPPLE_PAD_NORTH;
    if (e & (1u << NAPPLE_PAD_NORTH)) keep |= 1u << NAPPLE_PAD_WEST;
    return keep;
}

static void map_slot(const napple_input *in, int slot, napple_npad_state *st) {
    memset(st, 0, sizeof *st);
    float kx = 0, ky = 0, krx = 0, kry = 0;
    if (slot == 0) {   /* keyboard drives player 1 */
        for (int b = 0; b < NAPPLE_BTN_COUNT_BASIC; b++) if (held(in, in->bind.key[b][0]) || held(in, in->bind.key[b][1])) st->buttons |= NAPPLE_BTN_MASK(b);
        kx = (float)(held(in, in->bind.stick_l[2]) - held(in, in->bind.stick_l[0])); ky = (float)(held(in, in->bind.stick_l[1]) - held(in, in->bind.stick_l[3]));
        krx = (float)(held(in, in->bind.stick_r[2]) - held(in, in->bind.stick_r[0])); kry = (float)(held(in, in->bind.stick_r[1]) - held(in, in->bind.stick_r[3]));
        /* diagonal keys read as a unit vector, not as 1.41 */
        float m = sqrtf(kx * kx + ky * ky); if (m > 1) { kx /= m; ky /= m; }
        m = sqrtf(krx * krx + kry * kry); if (m > 1) { krx /= m; kry /= m; }
    }
    float lx = kx, ly = ky, rx = krx, ry = kry;
    if (slot == 0) {   /* on-screen controller (touch devices): the larger deflection wins, like a physical pad */
        st->buttons |= in->virt_buttons;
        if (in->virt_lx * in->virt_lx + in->virt_ly * in->virt_ly > lx * lx + ly * ly) { lx = in->virt_lx; ly = in->virt_ly; }
        if (in->virt_rx * in->virt_rx + in->virt_ry * in->virt_ry > rx * rx + ry * ry) { rx = in->virt_rx; ry = in->virt_ry; }
    }
    if (slot < in->npads) {
        const ng_pad *p = &in->pads[slot]; uint32_t e = p->elements;
        if ((in->bind.face_layout == 1 && p->by_label) || (in->bind.face_layout == 2 && !p->by_label)) e = swap_face(e);
        /* analog triggers: the press point is the player's setting, not GameController's */
        e &= ~((1u << NAPPLE_PAD_LT) | (1u << NAPPLE_PAD_RT));
        if (p->lt > in->bind.trigger_threshold) e |= 1u << NAPPLE_PAD_LT;
        if (p->rt > in->bind.trigger_threshold) e |= 1u << NAPPLE_PAD_RT;
        for (int b = 0; b < NAPPLE_BTN_COUNT_BASIC; b++) for (int k = 0; k < 2; k++) { int el = in->bind.pad[b][k]; if (el >= 0 && (e >> el & 1)) st->buttons |= NAPPLE_BTN_MASK(b); }
        float px, py, qx, qy; radial(p->lx, p->ly, in->bind.stick_deadzone, in->bind.stick_range, in->bind.stick_curve, &px, &py); radial(p->rx, p->ry, in->bind.stick_deadzone, in->bind.stick_range, in->bind.stick_curve, &qx, &qy);
        if (px * px + py * py > lx * lx + ly * ly) { lx = px; ly = py; }
        if (qx * qx + qy * qy > rx * rx + ry * ry) { rx = qx; ry = qy; }
    }
    if (in->bind.invert[0]) lx = -lx;
    if (in->bind.invert[1]) ly = -ly;
    if (in->bind.invert[2]) rx = -rx;
    if (in->bind.invert[3]) ry = -ry;
    st->left = to_stick(lx, ly); st->right = to_stick(rx, ry);
    /* stick direction pseudo buttons beyond half deflection */
    const int32_t half = (int32_t)(STICK_MAX / 2);
    if (st->left.x < -half) st->buttons |= NAPPLE_BTN_MASK(NAPPLE_BTN_STICK_L_LEFT);
    if (st->left.y > half) st->buttons |= NAPPLE_BTN_MASK(NAPPLE_BTN_STICK_L_UP);
    if (st->left.x > half) st->buttons |= NAPPLE_BTN_MASK(NAPPLE_BTN_STICK_L_RIGHT);
    if (st->left.y < -half) st->buttons |= NAPPLE_BTN_MASK(NAPPLE_BTN_STICK_L_DOWN);
    if (st->right.x < -half) st->buttons |= NAPPLE_BTN_MASK(NAPPLE_BTN_STICK_R_LEFT);
    if (st->right.y > half) st->buttons |= NAPPLE_BTN_MASK(NAPPLE_BTN_STICK_R_UP);
    if (st->right.x > half) st->buttons |= NAPPLE_BTN_MASK(NAPPLE_BTN_STICK_R_RIGHT);
    if (st->right.y < -half) st->buttons |= NAPPLE_BTN_MASK(NAPPLE_BTN_STICK_R_DOWN);
}

static uint32_t choose_style(uint32_t supported, int pref, int *handheld_only) {
    *handheld_only = 0;
    { static int ov = -2; if (ov == -2) { const char *e = getenv("NAPPLE_HID_STYLE"); ov = e ? (int)strtol(e, NULL, 0) : -1; } if (ov > 0) { if (ov == NAPPLE_STYLE_HANDHELD) *handheld_only = 1; return (uint32_t)ov; } }
    if (!supported) supported = NAPPLE_STYLE_FULL_KEY | NAPPLE_STYLE_HANDHELD | NAPPLE_STYLE_JOY_DUAL;
    /* the player's preference wins when the game supports that style; otherwise fall through to the automatic order */
    if (pref == NAPPLE_STYLE_PREF_PRO && (supported & NAPPLE_STYLE_FULL_KEY)) return NAPPLE_STYLE_FULL_KEY;
    if (pref == NAPPLE_STYLE_PREF_JOYCON && (supported & NAPPLE_STYLE_JOY_DUAL)) return NAPPLE_STYLE_JOY_DUAL;
    if (pref == NAPPLE_STYLE_PREF_HANDHELD && (supported & NAPPLE_STYLE_HANDHELD)) { *handheld_only = 1; return NAPPLE_STYLE_HANDHELD; }
    if (supported & NAPPLE_STYLE_FULL_KEY) return NAPPLE_STYLE_FULL_KEY;
    if (supported & NAPPLE_STYLE_JOY_DUAL) return NAPPLE_STYLE_JOY_DUAL;
    if (supported & NAPPLE_STYLE_HANDHELD) { *handheld_only = 1; return NAPPLE_STYLE_HANDHELD; }
    if (supported & NAPPLE_STYLE_JOY_LEFT) return NAPPLE_STYLE_JOY_LEFT;
    if (supported & NAPPLE_STYLE_JOY_RIGHT) return NAPPLE_STYLE_JOY_RIGHT;
    return NAPPLE_STYLE_FULL_KEY;
}
static uint32_t attrs_for(uint32_t style) {
    switch (style) {
    case NAPPLE_STYLE_HANDHELD: return NAPPLE_ATTR_CONNECTED | NAPPLE_ATTR_LEFT_CONNECTED | NAPPLE_ATTR_RIGHT_CONNECTED | NAPPLE_ATTR_WIRED | NAPPLE_ATTR_LEFT_WIRED | NAPPLE_ATTR_RIGHT_WIRED;
    case NAPPLE_STYLE_JOY_DUAL: return NAPPLE_ATTR_CONNECTED | NAPPLE_ATTR_LEFT_CONNECTED | NAPPLE_ATTR_RIGHT_CONNECTED;
    case NAPPLE_STYLE_JOY_LEFT: return NAPPLE_ATTR_CONNECTED | NAPPLE_ATTR_LEFT_CONNECTED;
    case NAPPLE_STYLE_JOY_RIGHT: return NAPPLE_ATTR_CONNECTED | NAPPLE_ATTR_RIGHT_CONNECTED;
    default: return NAPPLE_ATTR_CONNECTED;
    }
}

/* ------------------------------------------------------------------ update */
void napple_input_update(napple_input *in, uint64_t now) {
    ng_pad pads[NG_MAX_PADS]; int n = 0;
    if (in->cfg.use_game_controller && !getenv("NAPPLE_INPUT_NO_GC")) n = ng_snapshot(pads, NG_MAX_PADS);
    pthread_mutex_lock(&in->lock);
    uint8_t *shm = in->cfg.shm; const napple_hid_layout *l = &in->layout;
    const int neutral = in->blocked || !in->focused;
    in->npads = n; memcpy(in->pads, pads, sizeof pads);
    if (neutral) for (int i = 0; i < n; i++) { in->pads[i].elements = 0; in->pads[i].lx = in->pads[i].ly = in->pads[i].rx = in->pads[i].ry = 0; }
    uint64_t dt_ns = in->last_ns ? now - in->last_ns : 1000000000ull / (uint64_t)in->cfg.update_hz; in->last_ns = now;
    if (dt_ns > 200000000ull) dt_ns = 200000000ull;
    const float dt = (float)dt_ns / 1e9f;
    if (in->shake_req) { in->shake_req = 0; in->shake_start_ns = now ? now : 1; }
    int handheld_only; const uint32_t style = choose_style(in->supported_styles, in->bind.style, &handheld_only);
    napple_capture_fn cb = NULL; void *cbu = NULL; int cap_code = -1;
    for (int slot = 0; slot < 8; slot++) {
        const int present = slot == 0 || (slot < in->npads && slot < in->player_count);
        if (!present) {
            if (in->slot_style[slot]) { napple_npad_state off = {0}; napple_hid_push_npad(shm, l, slot, (uint32_t)in->slot_style[slot], &off); napple_hid_set_npad_style(shm, l, slot, 0, in->assign_mode); in->slot_style[slot] = 0; }
            continue;
        }
        napple_npad_state st; map_slot(in, slot, &st);
        st.attributes = attrs_for(style);
        in->pad_state[slot] = st;
        if (in->slot_style[slot] != (int)style) {
            if (in->slot_style[slot]) { napple_npad_state off = {0}; napple_hid_push_npad(shm, l, slot, (uint32_t)in->slot_style[slot], &off); }
            in->slot_style[slot] = (int)style;
            napple_hid_set_npad_style(shm, l, slot, style, in->assign_mode);
            napple_hid_set_npad_colors(shm, l, slot, 0x00828282 /* grey body */, 0x00ffffff);
        }
        napple_hid_push_npad(shm, l, slot, style, &st);
        if (handheld_only && slot == 0) {   /* handheld-only titles read npad id 0x20 */
            if (in->slot_style[9] != (int)style) { in->slot_style[9] = (int)style; napple_hid_set_npad_style(shm, l, 9, style, in->assign_mode); }
            napple_hid_push_npad(shm, l, 9, style, &st);
        }
        /* six-axis: controller motion if present, otherwise the at-rest pose */
        napple_sixaxis_state six; const float rest_acc[3] = {0, 0, -1}, zero[3] = {0, 0, 0};
        float shake_acc[3] = {0, 0, -1};
        int shaking = slot == 0 && in->shake_start_ns && in->sixaxis_enabled;
        if (shaking) {   /* SHAKE_CYCLES whole cycles of +-4 G along x, so the sensor ends at rest (SMO's cap throw, nxnative DM_BTN_SHAKE) */
            double t = (double)(now - in->shake_start_ns) * 1e-9;
            if (t >= SHAKE_CYCLES / SHAKE_HZ) in->shake_start_ns = 0, shaking = 0;
            else shake_acc[0] = (float)(4.0 * sin(2.0 * M_PI * SHAKE_HZ * t));
        }
        if (shaking) {
            napple_motion_step(&in->motion[slot], dt, shake_acc, zero, &six);
        } else if (slot < in->npads && in->pads[slot].has_motion && in->sixaxis_enabled) {
            napple_motion_step(&in->motion[slot], dt, in->pads[slot].acc, in->pads[slot].gyro, &six);
        } else {
            napple_motion_reset(&in->motion[slot]);
            napple_motion_step(&in->motion[slot], dt, rest_acc, zero, &six);
        }
        const int six_idx = style == NAPPLE_STYLE_FULL_KEY ? 0 : style == NAPPLE_STYLE_HANDHELD ? 1 : style == NAPPLE_STYLE_JOY_DUAL ? 2 : style == NAPPLE_STYLE_JOY_LEFT ? 4 : 5;
        napple_hid_push_sixaxis(shm, l, slot, six_idx, &six);
        if (style == NAPPLE_STYLE_JOY_DUAL) napple_hid_push_sixaxis(shm, l, slot, 3, &six);
        if (handheld_only && slot == 0) napple_hid_push_sixaxis(shm, l, 9, 1, &six);
        napple_hid_set_npad_device(shm, l, slot, 0, 0, slot < in->npads ? (uint32_t)in->pads[slot].battery : 4u);
    }
    /* capture next controller press for the remap UI */
    if (in->capture) {
        for (int i = 0; i < n && cap_code < 0; i++) {
            uint32_t rising = pads[i].elements & ~in->prev_elements[i];
            if (rising) { for (int el = 0; el < NAPPLE_PAD_COUNT; el++) if (rising >> el & 1) { cap_code = el; break; } }
        }
        if (cap_code >= 0) { cb = in->capture; cbu = in->capture_user; in->capture = NULL; }
    }
    for (int i = 0; i < NG_MAX_PADS; i++) in->prev_elements[i] = i < n ? pads[i].elements : 0;
    int hot = 0;   /* Home / Share edges from any controller (not gated by blocked: the overlay opens and closes with them) */
    for (int i = 0; i < NG_MAX_PADS; i++) {
        uint32_t cur = i < n && in->focused ? pads[i].hot : 0, rise = cur & ~in->prev_hot[i];
        if (rise & 1u) hot = NAPPLE_HOTKEY_HOME; else if ((rise & 2u) && !hot) hot = NAPPLE_HOTKEY_CAPTURE;
        in->prev_hot[i] = cur;
    }
    napple_hotkey_fn hk = in->hotkey; void *hku = in->hotkey_user;

    /* touch screen from the mouse */
    const int lmb = !neutral && in->bind.mouse_touch && (in->mouse_buttons & 1) && in->mouse_have_pos;
    napple_touch_point tp = {.finger_id = 0, .x = (uint32_t)(in->mouse_nx * (float)(in->cfg.screen_w - 1)), .y = (uint32_t)(in->mouse_ny * (float)(in->cfg.screen_h - 1)), .diameter_x = 15, .diameter_y = 15};
    if (lmb) { napple_hid_push_touch(shm, l, &tp, 1); in->snap.touch_x = tp.x; in->snap.touch_y = tp.y; }
    else if (in->touch_down) napple_hid_push_touch(shm, l, NULL, 0);
    in->touch_down = lmb; in->snap.touch_active = lmb;

    /* mouse */
    napple_mouse_state ms = {.x = (int32_t)(in->mouse_nx * (float)(in->cfg.screen_w - 1)), .y = (int32_t)(in->mouse_ny * (float)(in->cfg.screen_h - 1)),
                             .buttons = neutral ? 0 : (uint32_t)in->mouse_buttons & 0x1f, .attributes = 3};
    ms.delta_x = in->mouse_have_pos ? ms.x - in->last_mx : 0; ms.delta_y = in->mouse_have_pos ? ms.y - in->last_my : 0;
    ms.wheel_x = (int32_t)lroundf(in->scroll_dx); ms.wheel_y = (int32_t)lroundf(in->scroll_dy); in->scroll_dx = in->scroll_dy = 0;
    in->last_mx = ms.x; in->last_my = ms.y;
    if (memcmp(&ms, &in->prev_mouse, sizeof ms) || ms.buttons) { napple_hid_push_mouse(shm, l, &ms); in->prev_mouse = ms; }

    /* keyboard (usb usage bitmap + modifiers) */
    uint8_t kb[32] = {0}; uint64_t mods = 0;
    if (!neutral) for (int k = 0; k < 256; k++) if (in->keys[k]) {
        uint8_t u = napple_mac_to_hid_usage((uint16_t)k); uint64_t mb = napple_mac_modifier_bit((uint16_t)k);
        if (u && u < 0xE0) kb[u / 8] |= (uint8_t)(1u << (u % 8)); else if (u >= 0xE0) kb[u / 8] |= (uint8_t)(1u << (u % 8));
        mods |= mb;
    }
    if (memcmp(kb, in->prev_keyboard, 32) || mods != in->prev_modifiers) { napple_hid_push_keyboard(shm, l, mods, kb); memcpy(in->prev_keyboard, kb, 32); in->prev_modifiers = mods; }

    /* NAPPLE_INPUT_LOG=1: log every change of player 1's merged state (end-to-end input tests, see docs) */
    { static int on = -1; static uint64_t last_b; static int32_t last_lx, last_ly, last_rx, last_ry;
      if (on < 0) on = getenv("NAPPLE_INPUT_LOG") != NULL;
      const napple_npad_state *p0 = &in->pad_state[0];
      if (on && (p0->buttons != last_b || p0->left.x != last_lx || p0->left.y != last_ly || p0->right.x != last_rx || p0->right.y != last_ry)) {
          char names[200] = ""; for (int b = 0; b < NAPPLE_BTN_COUNT_BASIC; b++) if (p0->buttons >> b & 1) { strlcat(names, napple_button_name(b), sizeof names); strlcat(names, " ", sizeof names); }
          napple_input_log("player1 buttons=%#llx [%s] L=(%d,%d) R=(%d,%d) focused=%d blocked=%d", (unsigned long long)p0->buttons, names, p0->left.x, p0->left.y, p0->right.x, p0->right.y, in->focused, in->blocked);
          last_b = p0->buttons; last_lx = p0->left.x; last_ly = p0->left.y; last_rx = p0->right.x; last_ry = p0->right.y; } }
    /* snapshot for UI / tests */
    in->snap.pad = in->pad_state[0]; in->snap.controllers = n; in->snap.controller_name[0] = 0;
    if (n) strlcpy(in->snap.controller_name, pads[0].name, sizeof in->snap.controller_name);
    for (int i = 0; i < 8; i++) { in->snap.controller_names[i][0] = 0; if (i < n) strlcpy(in->snap.controller_names[i], pads[i].name, sizeof in->snap.controller_names[i]); in->snap.player_style[i] = (uint32_t)in->slot_style[i]; }
    pthread_mutex_unlock(&in->lock);
    if (cb) cb(cbu, 0, cap_code);
    if (hot && hk) hk(hku, hot);
}

void napple_input_get_snapshot(napple_input *in, napple_input_snapshot *out) { pthread_mutex_lock(&in->lock); *out = in->snap; pthread_mutex_unlock(&in->lock); }
