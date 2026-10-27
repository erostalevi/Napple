/* HID shared memory writer: pure functions over a byte buffer (no OS calls), so they are unit testable.

   Ring ("lifo") format used by every hid shared memory section:
     header (0x20): u64 timestamp, u64 buffer_count (17), u64 tail (index of the newest entry), u64 count (valid entries)
     entries (17): u64 sampling_number (outer, written last, lets readers detect a torn entry) + state bytes
   The OUTER sampling number is written as 2*n and the state's own (inner) number as n: the Nintendo SDK's reader (seen in Animal Crossing 3.0.3, nn::hid
   Npad lifo read) treats an odd outer number as "entry being written" and retries forever, and requires consecutive inner numbers (difference 1).
   A state itself starts with its own u64 sampling_number (npad, mouse, keyboard, touch) or with delta_time then
   sampling_number (six-axis).  The writer advances `tail`, fills the entry, then publishes `count`/`tail` with
   release ordering so a game thread reading concurrently sees either the old or the new entry.

   Struct sizes that were verified by construction (their totals close the documented section sizes):
     npad state 0x28 (entry 0x30), six-axis state 0x60 (entry 0x68), touch state 0x290 (entry 0x298; 17 entries + 0x20
     header + 0x3C8 padding = 0x3000), mouse state 0x28, keyboard state 0x30.
   Offsets of the Npad entry fields past the first lifo follow Switchbrew/libnx/yuzu documentation and are table
   driven (napple_hid_layout): see docs/architecture/audio-input-frontend.md section 2.4. */
#include "napple_input.h"
#include "gesture.h"
#include <stdatomic.h>
#include <string.h>

#define RING 17u
#define NPAD_STATE 0x28u
#define SIX_STATE 0x60u
#define TOUCH_STATE 0x290u
#define MOUSE_STATE 0x28u
#define KEY_STATE 0x30u
#define NPAD_ENTRY_SIZE 0x5000u
#define DEBUGPAD_STATE 0x20u
#define GESTURE_STATE 0x60u

uint32_t napple_hid_lifo_bytes(uint32_t state_size) { return 0x20 + RING * (8 + state_size); }

int napple_npad_slot_from_id(uint32_t id) {
    if (id <= 7) return (int)id;
    if (id == NAPPLE_NPAD_OTHER) return 8;
    if (id == NAPPLE_NPAD_HANDHELD) return 9;
    return -1;
}

void napple_hid_layout_for(int profile, napple_hid_layout *l) {
    memset(l, 0, sizeof *l);
    l->debug_pad = 0x0;
    l->touch_screen = 0x400;
    l->mouse = 0x3400;
    l->keyboard = 0x3800;
    l->npad_base = 0x9A00;
    l->npad_stride = NPAD_ENTRY_SIZE;
    l->lifo_count = RING;
    const uint32_t lifo = napple_hid_lifo_bytes(NPAD_STATE), six = napple_hid_lifo_bytes(SIX_STATE);
    uint32_t at = 0x28; /* style(4) assignment(4) full-key colour(0xC) joy colours(0x14) */
    const int lifos = profile == NAPPLE_HID_LAYOUT_COMPAT ? 5 : 7;
    for (int i = 0; i < lifos; i++, at += lifo) l->npad_lifo[i] = at;
    for (int i = 0; i < 6; i++, at += six) l->npad_sixaxis[i] = at;
    l->npad_device_type = at; /* u32; +4 reserved; +8 u64 system properties */
    l->npad_system_properties = at + 8;
    l->npad_button_properties = at + 16;
    l->npad_battery = at + 20;
    l->revision = (uint32_t)profile;
    l->gesture = l->npad_base + 10 * l->npad_stride; /* right after the ten npad entries; section size 0x800 */
    l->console_sixaxis = l->gesture + 0x800;
}

/* ---- small helpers ---- */
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static void putf(uint8_t *p, float v) { memcpy(p, &v, 4); }
static uint64_t get64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

/* Start a new entry in the ring at `ring`: returns the pointer to the entry's state bytes and writes the sampling
   number; finish with ring_commit. */
typedef struct {
    uint8_t *state;
    uint64_t sampling;
    uint32_t tail, count;
    uint8_t *hdr;
    uint8_t *entry;
} Slot;
static Slot ring_begin(uint8_t *shm, uint32_t ring, uint32_t state_size) {
    Slot s;
    s.hdr = shm + ring;
    uint64_t count = get64(s.hdr + 24), tail = get64(s.hdr + 16);
    if (get64(s.hdr + 8) != RING) {
        put64(s.hdr + 8, RING);
        count = 0;
        tail = RING - 1;
    }
    uint32_t next = count ? (uint32_t)((tail + 1) % RING) : 0;
    s.sampling = count ? get64(s.hdr + 0x20 + tail * (8 + state_size)) / 2 + 1 : 0; /* the stored outer number is 2*n */
    s.tail = next;
    s.count = (uint32_t)(count < RING ? count + 1 : RING);
    s.entry = s.hdr + 0x20 + (size_t)next * (8 + state_size);
    s.state = s.entry + 8;
    memset(s.state, 0, state_size);
    return s;
}
static void ring_commit(Slot *s) {
    put64(s->entry, s->sampling * 2); /* outer sampling number (even: odd means "being written" to the SDK reader) */
    put64(s->state, s->sampling);     /* the state's own copy (not used by six-axis: see caller) */
    atomic_thread_fence(memory_order_release);
    put64(s->hdr + 8, RING);
    put64(s->hdr + 24, s->count);
    put64(s->hdr + 16, s->tail);
    put64(s->hdr + 0, s->sampling);
}

/* A ring starts full of idle entries with consecutive sampling numbers 0..RING-1 (outer copy 2*n; tail = newest): games read a whole history (SMO reads all 16
   entries of GetNpadStates and ignores the returned count) and an entry with a stale or zero sampling number reads as "no sample", which left
   its sticks dead until the ring had wrapped. Zero states carry valid numbers; the first push continues at RING. nxnative docs/reference/services.md
   "History reads fill every requested entry" (MIT; behaviour only). */
static void ring_prefill(uint8_t *shm, uint32_t ring, uint32_t state_size, int six) {
    uint8_t *hdr = shm + ring;
    for (uint32_t i = 0; i < RING; i++) {
        uint8_t *e = hdr + 0x20 + (size_t)i * (8 + state_size);
        put64(e, 2 * (uint64_t)i);
        put64(e + 8 + (six ? 8 : 0), i); /* outer number is 2*n (even), see ring_commit */
    }
    put64(hdr + 0, RING - 1);
    put64(hdr + 8, RING);
    put64(hdr + 16, RING - 1);
    put64(hdr + 24, RING);
}

void napple_hid_init(uint8_t *shm, const napple_hid_layout *l) {
    memset(shm, 0, NAPPLE_HID_SHM_SIZE);
    ring_prefill(shm, l->debug_pad, DEBUGPAD_STATE, 0);
    ring_prefill(shm, l->touch_screen, TOUCH_STATE, 0);
    if (l->gesture) ring_prefill(shm, l->gesture, GESTURE_STATE, 0);
    ring_prefill(shm, l->mouse, MOUSE_STATE, 0);
    ring_prefill(shm, l->keyboard, KEY_STATE, 0);
    for (int slot = 0; slot < 10; slot++) {
        uint32_t base = l->npad_base + (uint32_t)slot * l->npad_stride;
        for (int i = 0; i < 7; i++)
            if (l->npad_lifo[i]) ring_prefill(shm, base + l->npad_lifo[i], NPAD_STATE, 0);
        for (int i = 0; i < 6; i++)
            if (l->npad_sixaxis[i]) ring_prefill(shm, base + l->npad_sixaxis[i], SIX_STATE, 1);
    }
}

void napple_hid_set_npad_style(uint8_t *shm, const napple_hid_layout *l, int slot, uint32_t style_set, uint32_t assign) {
    if (slot < 0 || slot > 9) return;
    uint8_t *b = shm + l->npad_base + (uint32_t)slot * l->npad_stride;
    put32(b + 0, style_set);
    put32(b + 4, assign);
}

void napple_hid_set_npad_colors(uint8_t *shm, const napple_hid_layout *l, int slot, uint32_t body, uint32_t buttons) {
    if (slot < 0 || slot > 9) return;
    uint8_t *b = shm + l->npad_base + (uint32_t)slot * l->npad_stride;
    put32(b + 8, 0);
    put32(b + 12, body);
    put32(b + 16, buttons); /* full key: attribute ok, main, sub */
    put32(b + 0x14, 0);
    put32(b + 0x18, body);
    put32(b + 0x1c, buttons);
    put32(b + 0x20, body);
    put32(b + 0x24, buttons); /* joy colours */
}

static int style_index(uint32_t style) {
    switch (style) {
        case NAPPLE_STYLE_FULL_KEY: return 0;
        case NAPPLE_STYLE_HANDHELD: return 1;
        case NAPPLE_STYLE_JOY_DUAL: return 2;
        case NAPPLE_STYLE_JOY_LEFT: return 3;
        case NAPPLE_STYLE_JOY_RIGHT: return 4;
        case NAPPLE_STYLE_PALMA: return 5;
        case NAPPLE_STYLE_SYSTEM_EXT:
        case NAPPLE_STYLE_SYSTEM: return 6;
        default: return -1;
    }
}

static void push_npad_lifo(uint8_t *shm, const napple_hid_layout *l, int slot, int idx, const napple_npad_state *st);
void napple_hid_push_npad(uint8_t *shm, const napple_hid_layout *l, int slot, uint32_t style, const napple_npad_state *st) {
    int idx = style_index(style);
    if (idx >= 0 && idx < 5 && l->npad_lifo[6]) push_npad_lifo(shm, l, slot, 6, st); /* the system (firmware applet) reads the controller through the SystemExt lifo too */
    push_npad_lifo(shm, l, slot, idx, st);
}
static void push_npad_lifo(uint8_t *shm, const napple_hid_layout *l, int slot, int idx, const napple_npad_state *st) {
    if (slot < 0 || slot > 9 || idx < 0 || !l->npad_lifo[idx]) return;
    uint32_t ring = l->npad_base + (uint32_t)slot * l->npad_stride + l->npad_lifo[idx];
    Slot s = ring_begin(shm, ring, NPAD_STATE);
    put64(s.state + 8, st->buttons);
    put32(s.state + 0x10, (uint32_t)st->left.x);
    put32(s.state + 0x14, (uint32_t)st->left.y);
    put32(s.state + 0x18, (uint32_t)st->right.x);
    put32(s.state + 0x1c, (uint32_t)st->right.y);
    put32(s.state + 0x20, st->attributes);
    ring_commit(&s);
}

void napple_hid_push_sixaxis(uint8_t *shm, const napple_hid_layout *l, int slot, int idx, const napple_sixaxis_state *st) {
    if (slot < 0 || slot > 9 || idx < 0 || idx > 5 || !l->npad_sixaxis[idx]) return;
    uint32_t ring = l->npad_base + (uint32_t)slot * l->npad_stride + l->npad_sixaxis[idx];
    Slot s = ring_begin(shm, ring, SIX_STATE);
    put64(s.state + 0, st->delta_time_ns); /* six-axis state: delta_time first, then sampling number */
    for (int i = 0; i < 3; i++) {
        putf(s.state + 0x10 + 4 * i, st->acceleration[i]);
        putf(s.state + 0x1c + 4 * i, st->angular_velocity[i]);
        putf(s.state + 0x28 + 4 * i, st->angle[i]);
    }
    for (int i = 0; i < 9; i++) putf(s.state + 0x34 + 4 * i, st->direction[i]);
    put32(s.state + 0x58, st->attributes);
    atomic_thread_fence(memory_order_release);
    put64(s.entry, s.sampling * 2);
    put64(s.state + 8, s.sampling);
    put64(s.hdr + 8, RING);
    put64(s.hdr + 24, s.count);
    put64(s.hdr + 16, s.tail);
    put64(s.hdr + 0, s.sampling);
}

void napple_hid_set_npad_device(uint8_t *shm, const napple_hid_layout *l, int slot, uint32_t device_type, uint64_t props, uint32_t battery) {
    if (slot < 0 || slot > 9 || !l->npad_device_type) return;
    uint8_t *b = shm + l->npad_base + (uint32_t)slot * l->npad_stride;
    put32(b + l->npad_device_type, device_type);
    put64(b + l->npad_system_properties, props);
    put32(b + l->npad_battery, battery);
    put32(b + l->npad_battery + 4, battery);
    put32(b + l->npad_battery + 8, battery);
}

void napple_hid_push_touch(uint8_t *shm, const napple_hid_layout *l, const napple_touch_point *pts, uint32_t count) {
    if (count > 16) count = 16;
    Slot s = ring_begin(shm, l->touch_screen, TOUCH_STATE);
    put32(s.state + 8, count);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *t = s.state + 0x10 + 0x28 * i;
        put32(t + 8, 1 /* start */);
        put32(t + 0xc, pts[i].finger_id);
        put32(t + 0x10, pts[i].x);
        put32(t + 0x14, pts[i].y);
        put32(t + 0x18, pts[i].diameter_x);
        put32(t + 0x1c, pts[i].diameter_y);
        put32(t + 0x20, pts[i].rotation_angle);
    }
    ring_commit(&s);
}

void napple_hid_push_debug_pad(uint8_t *shm, const napple_hid_layout *l, const napple_debug_pad_state *d) {
    Slot s = ring_begin(shm, l->debug_pad, DEBUGPAD_STATE);
    put32(s.state + 8, d->attributes);
    put32(s.state + 12, d->buttons);
    put32(s.state + 16, (uint32_t)d->right.x);
    put32(s.state + 20, (uint32_t)d->right.y);
    put32(s.state + 24, (uint32_t)d->left.x);
    put32(s.state + 28, (uint32_t)d->left.y);
    ring_commit(&s);
}

void napple_debug_pad_from_npad(const napple_npad_state *in, napple_debug_pad_state *out) {
    static const struct {
        int npad_button;
        int debug_bit;
    } map[] = {{NAPPLE_BTN_A, 0},     {NAPPLE_BTN_B, 1},   {NAPPLE_BTN_X, 2},      {NAPPLE_BTN_Y, 3},    {NAPPLE_BTN_L, 4},
               {NAPPLE_BTN_R, 5},     {NAPPLE_BTN_ZL, 6},  {NAPPLE_BTN_ZR, 7},     {NAPPLE_BTN_PLUS, 8}, {NAPPLE_BTN_MINUS, 9},
               {NAPPLE_BTN_LEFT, 10}, {NAPPLE_BTN_UP, 11}, {NAPPLE_BTN_RIGHT, 12}, {NAPPLE_BTN_DOWN, 13}};
    memset(out, 0, sizeof *out);
    out->attributes = NAPPLE_DEBUGPAD_ATTR_CONNECTED;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (in->buttons & NAPPLE_BTN_MASK(map[i].npad_button)) out->buttons |= 1u << map[i].debug_bit;
    out->left = in->left;
    out->right = in->right;
}

void napple_hid_push_gesture(uint8_t *shm, const napple_hid_layout *l, const struct napple_gesture_state *g) {
    if (!l->gesture) return;
    Slot s = ring_begin(shm, l->gesture, GESTURE_STATE);
    put64(s.state + 8, g->context_number);
    put32(s.state + 16, g->type);
    put32(s.state + 20, g->direction);
    put32(s.state + 24, (uint32_t)g->x);
    put32(s.state + 28, (uint32_t)g->y);
    put32(s.state + 32, (uint32_t)g->delta_x);
    put32(s.state + 36, (uint32_t)g->delta_y);
    putf(s.state + 40, g->velocity_x);
    putf(s.state + 44, g->velocity_y);
    put32(s.state + 48, g->attributes);
    putf(s.state + 52, g->scale);
    putf(s.state + 56, g->rotation_angle);
    put32(s.state + 60, (uint32_t)g->point_count);
    for (int i = 0; i < NAPPLE_GESTURE_MAX_POINTS; i++) {
        put32(s.state + 64 + 8 * i, (uint32_t)g->points[i].x);
        put32(s.state + 68 + 8 * i, (uint32_t)g->points[i].y);
    }
    ring_commit(&s);
}

void napple_hid_set_console_sixaxis(uint8_t *shm, const napple_hid_layout *l, int at_rest, float vert_err, const float bias[3]) {
    if (!l->console_sixaxis) return;
    uint8_t *b = shm + l->console_sixaxis;
    uint64_t n = get64(b) + 1;
    put64(b, n);
    b[8] = at_rest ? 1 : 0;
    putf(b + 12, vert_err);
    for (int i = 0; i < 3; i++) putf(b + 16 + 4 * i, bias ? bias[i] : 0.0f);
}

void napple_hid_push_mouse(uint8_t *shm, const napple_hid_layout *l, const napple_mouse_state *m) {
    Slot s = ring_begin(shm, l->mouse, MOUSE_STATE);
    put32(s.state + 8, (uint32_t)m->x);
    put32(s.state + 12, (uint32_t)m->y);
    put32(s.state + 16, (uint32_t)m->delta_x);
    put32(s.state + 20, (uint32_t)m->delta_y);
    put32(s.state + 24, (uint32_t)m->wheel_x);
    put32(s.state + 28, (uint32_t)m->wheel_y);
    put32(s.state + 32, m->buttons);
    put32(s.state + 36, m->attributes);
    ring_commit(&s);
}

void napple_hid_push_keyboard(uint8_t *shm, const napple_hid_layout *l, uint64_t modifiers, const uint8_t keys[32]) {
    Slot s = ring_begin(shm, l->keyboard, KEY_STATE);
    put64(s.state + 8, modifiers);
    memcpy(s.state + 16, keys, 32);
    ring_commit(&s);
}

const uint8_t *napple_hid_lifo_latest(const uint8_t *shm, uint32_t ring, uint32_t state_size) {
    uint64_t count = get64(shm + ring + 24), tail = get64(shm + ring + 16);
    if (!count || tail >= RING) return NULL;
    return shm + ring + 0x20 + tail * (8 + state_size) + 8;
}
uint64_t napple_hid_lifo_count(const uint8_t *shm, uint32_t ring) { return get64(shm + ring + 24); }
