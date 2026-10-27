/* napple_input.h - host side of Horizon HID for Napple.

   Two layers:
     1. HID shared memory writer (pure functions over a byte buffer, no OS calls): napple_hid_*.
        The runtime owns the 0x40000-byte shared memory block the guest maps through hid:
        GetSharedMemoryHandle; this layer fills it.
     2. The input backend (napple_input): keyboard / mouse / macOS GameController -> Switch controller
        state, remapping persisted per game, rumble (CoreHaptics), motion (GCMotion), and a thread that
        keeps the shared memory fresh.

   See docs/architecture/audio-input-frontend.md. */
#ifndef NAPPLE_INPUT_H
#define NAPPLE_INPUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NAPPLE_INPUT_API_VERSION 1
#define NAPPLE_HID_SHM_SIZE 0x40000u

/* ===================================================================== */
/* Switch constants (values are the Horizon ABI)                         */
/* ===================================================================== */

/* HidNpadButton bit positions */
enum {
    NAPPLE_BTN_A = 0,
    NAPPLE_BTN_B,
    NAPPLE_BTN_X,
    NAPPLE_BTN_Y,
    NAPPLE_BTN_STICK_L,
    NAPPLE_BTN_STICK_R,
    NAPPLE_BTN_L,
    NAPPLE_BTN_R,
    NAPPLE_BTN_ZL,
    NAPPLE_BTN_ZR,
    NAPPLE_BTN_PLUS,
    NAPPLE_BTN_MINUS,
    NAPPLE_BTN_LEFT,
    NAPPLE_BTN_UP,
    NAPPLE_BTN_RIGHT,
    NAPPLE_BTN_DOWN,
    NAPPLE_BTN_STICK_L_LEFT,
    NAPPLE_BTN_STICK_L_UP,
    NAPPLE_BTN_STICK_L_RIGHT,
    NAPPLE_BTN_STICK_L_DOWN,
    NAPPLE_BTN_STICK_R_LEFT,
    NAPPLE_BTN_STICK_R_UP,
    NAPPLE_BTN_STICK_R_RIGHT,
    NAPPLE_BTN_STICK_R_DOWN,
    NAPPLE_BTN_LEFT_SL,
    NAPPLE_BTN_LEFT_SR,
    NAPPLE_BTN_RIGHT_SL,
    NAPPLE_BTN_RIGHT_SR,
    NAPPLE_BTN_COUNT_BASIC = 16, /* A..Down: the bindable "pad" buttons */
    NAPPLE_BTN_COUNT = 28
};
#define NAPPLE_BTN_MASK(b) (1ull << (b))

/* HidNpadStyleTag bits */
enum {
    NAPPLE_STYLE_FULL_KEY = 1u << 0, /* Pro Controller */
    NAPPLE_STYLE_HANDHELD = 1u << 1,
    NAPPLE_STYLE_JOY_DUAL = 1u << 2,
    NAPPLE_STYLE_JOY_LEFT = 1u << 3,
    NAPPLE_STYLE_JOY_RIGHT = 1u << 4,
    NAPPLE_STYLE_GC = 1u << 5,
    NAPPLE_STYLE_PALMA = 1u << 6,
    NAPPLE_STYLE_SYSTEM_EXT = 1u << 29,
    NAPPLE_STYLE_SYSTEM = 1u << 30
};

/* HidNpadAttribute bits (per state entry) */
enum {
    NAPPLE_ATTR_CONNECTED = 1u << 0,
    NAPPLE_ATTR_WIRED = 1u << 1,
    NAPPLE_ATTR_LEFT_CONNECTED = 1u << 2,
    NAPPLE_ATTR_LEFT_WIRED = 1u << 3,
    NAPPLE_ATTR_RIGHT_CONNECTED = 1u << 4,
    NAPPLE_ATTR_RIGHT_WIRED = 1u << 5
};

/* NpadIdType -> shared memory slot: players 1..8 = ids 0..7, no.9 (0x10 "Other") = slot 8, handheld (0x20) = slot 9. */
enum { NAPPLE_NPAD_PLAYER1 = 0, NAPPLE_NPAD_PLAYER8 = 7, NAPPLE_NPAD_OTHER = 0x10, NAPPLE_NPAD_HANDHELD = 0x20 };
int napple_npad_slot_from_id(uint32_t npad_id); /* -1 if the id is unknown */

/* ===================================================================== */
/* Layer 1: HID shared memory                                            */
/* ===================================================================== */

/* Offsets inside the Npad entry (0x5000 bytes) and top level. Filled by napple_hid_layout_for(); a game or
   firmware generation that differs can be described by editing this table (it is data, not code). */
typedef struct napple_hid_layout {
    uint32_t debug_pad, touch_screen, mouse, keyboard, npad_base, npad_stride;
    uint32_t npad_lifo[7];    /* full_key, handheld, joy_dual, joy_left, joy_right, palma, system_ext (0 = absent) */
    uint32_t npad_sixaxis[6]; /* full_key, handheld, joy_dual_left, joy_dual_right, joy_left, joy_right (0 = absent) */
    uint32_t npad_device_type, npad_system_properties, npad_button_properties, npad_battery, npad_gc_trigger;
    uint32_t lifo_count;               /* entries per ring: 17 */
    uint32_t revision;                 /* which profile this table came from (NAPPLE_HID_LAYOUT_*) */
    uint32_t gesture, console_sixaxis; /* gesture lifo section (0x800) and console six-axis record (0x20); offsets unverified */
} napple_hid_layout;

enum {
    NAPPLE_HID_LAYOUT_LATEST = 0, /* firmware 10.0.0+ style Npad entry (palma + system_ext lifos before six-axis) */
    NAPPLE_HID_LAYOUT_COMPAT = 1  /* five lifos then six-axis (games built for older SDKs reading older offsets)  */
};
void napple_hid_layout_for(int profile, napple_hid_layout *out);

typedef struct napple_stick {
    int32_t x, y;
} napple_stick; /* -32767..32767, y up */
typedef struct napple_npad_state {
    uint64_t buttons; /* NAPPLE_BTN_MASK bits */
    napple_stick left, right;
    uint32_t attributes; /* NAPPLE_ATTR_* */
} napple_npad_state;

typedef struct napple_sixaxis_state {
    uint64_t delta_time_ns;
    float acceleration[3];     /* G */
    float angular_velocity[3]; /* revolutions per second (Switch unit: 1.0 = 360 deg/s) */
    float angle[3];            /* integrated, revolutions */
    float direction[9];        /* orientation matrix */
    uint32_t attributes;       /* bit0 connected, bit1 interpolated */
} napple_sixaxis_state;

typedef struct napple_touch_point {
    uint32_t finger_id;
    uint32_t x, y; /* 0..1279 / 0..719 */
    uint32_t diameter_x, diameter_y;
    uint32_t rotation_angle;
} napple_touch_point;

typedef struct napple_mouse_state {
    int32_t x, y, delta_x, delta_y, wheel_x, wheel_y;
    uint32_t buttons;    /* bit0 left, bit1 right, bit2 middle, bit3 forward, bit4 back */
    uint32_t attributes; /* bit0 transferable, bit1 is connected */
} napple_mouse_state;

/* Initialise: zero the block, write ring headers. */
void napple_hid_init(uint8_t *shm, const napple_hid_layout *l);
/* Style set / assignment mode of one slot (0..9). */
void napple_hid_set_npad_style(uint8_t *shm, const napple_hid_layout *l, int slot, uint32_t style_set, uint32_t assignment_mode);
void napple_hid_set_npad_colors(uint8_t *shm, const napple_hid_layout *l, int slot, uint32_t body, uint32_t buttons);
/* Push one state into the lifo of `style` (exactly one NAPPLE_STYLE_* bit). Increments the sampling number. */
void napple_hid_push_npad(uint8_t *shm, const napple_hid_layout *l, int slot, uint32_t style, const napple_npad_state *s);
void napple_hid_push_sixaxis(uint8_t *shm, const napple_hid_layout *l, int slot, int sixaxis_index, const napple_sixaxis_state *s);
void napple_hid_set_npad_device(uint8_t *shm, const napple_hid_layout *l, int slot, uint32_t device_type, uint64_t system_properties, uint32_t battery_level);
void napple_hid_push_touch(uint8_t *shm, const napple_hid_layout *l, const napple_touch_point *pts, uint32_t count);
/* Debug pad (hid 1 ActivateDebugPad): DebugPadState {u64 sampling, u32 attributes (bit0 connected), u32 buttons (A B X Y L R ZL ZR Plus Minus Left Up Right Down = bits 0..13),
   right stick {s32 x, y}, left stick {s32 x, y}} in a 17 entry ring at layout.debug_pad. Layout is from the author's memory of the public description: unverified. */
enum { NAPPLE_DEBUGPAD_ATTR_CONNECTED = 1u };
typedef struct napple_debug_pad_state {
    uint32_t attributes, buttons;
    napple_stick right, left;
} napple_debug_pad_state;
void napple_hid_push_debug_pad(uint8_t *shm, const napple_hid_layout *l, const napple_debug_pad_state *s);
/* Maps a Switch npad state to the debug pad: the face and shoulder buttons keep their meaning, the d-pad and +/- too; sticks are copied. */
void napple_debug_pad_from_npad(const napple_npad_state *in, napple_debug_pad_state *out);
/* Gesture section: GestureState (see gesture.h) in a 17 entry ring at layout.gesture. */
struct napple_gesture_state;
void napple_hid_push_gesture(uint8_t *shm, const napple_hid_layout *l, const struct napple_gesture_state *g);
int napple_sixaxis_at_rest(const napple_sixaxis_state *s); /* 1 when the sensor is still: ~1 G of acceleration, negligible rotation */
/* Console six-axis record: {u64 sampling, u8 at rest, f32 verticalization error, f32 gyro bias x, y, z}. */
void napple_hid_set_console_sixaxis(uint8_t *shm, const napple_hid_layout *l, int at_rest, float verticalization_error, const float gyro_bias[3]);
void napple_hid_push_mouse(uint8_t *shm, const napple_hid_layout *l, const napple_mouse_state *s);
void napple_hid_push_keyboard(uint8_t *shm, const napple_hid_layout *l, uint64_t modifiers, const uint8_t keys[32]);
/* Reading (tests, diagnostics): latest entry of the ring at `ring` (offset of the lifo header); returns
   a pointer to the entry's state bytes (after the outer sampling number) or NULL when empty. */
const uint8_t *napple_hid_lifo_latest(const uint8_t *shm, uint32_t ring, uint32_t entry_size);
uint64_t napple_hid_lifo_count(const uint8_t *shm, uint32_t ring);
/* Size in bytes of one ring (header + count * (8 + state_size)) for a state of state_size bytes. */
uint32_t napple_hid_lifo_bytes(uint32_t state_size);

/* ===================================================================== */
/* Layer 2: input backend                                                */
/* ===================================================================== */

typedef struct napple_input napple_input;

typedef struct napple_input_config {
    const char *title_id;    /* 16 hex digits; selects ~/Library/Application Support/Napple/<titleid>/input.json */
    const char *config_dir;  /* override of the per-game directory (tests); NULL = default */
    uint8_t *shm;            /* HID shared memory block the backend keeps updated (0x40000 bytes); may be NULL */
    int layout_profile;      /* NAPPLE_HID_LAYOUT_* */
    int update_hz;           /* shared memory update rate; 0 = 120 */
    int use_game_controller; /* 1 = start GameController discovery (0 in headless tests) */
    int screen_w, screen_h;  /* touch screen space; 0 = 1280x720 */
} napple_input_config;

int napple_input_create(const napple_input_config *config, napple_input **out);
void napple_input_destroy(napple_input *in);
/* Start/stop the thread that samples and writes shared memory. */
int napple_input_start(napple_input *in);
void napple_input_stop(napple_input *in);
/* One synchronous sample + shm write (what the thread does; also for tests and for runtimes that prefer to
   drive it from their own timer). */
void napple_input_update(napple_input *in, uint64_t now_ns);

/* ---- hid IPC side: what the guest configures ---- */
void napple_input_set_supported_styles(napple_input *in, uint32_t style_set); /* SetSupportedNpadStyleSet */
void napple_input_set_assignment_mode(napple_input *in, uint32_t mode);       /* 0 dual, 1 single */
void napple_input_set_player_count(napple_input *in, int count);              /* NpadIdType slots used (1..8) */
uint32_t napple_input_active_style(const napple_input *in, int slot);         /* style the backend reports for slot */
/* Vibration (SendVibrationValue). Amplitudes 0..1, frequencies in Hz. */
typedef struct napple_vibration {
    float amp_low, freq_low, amp_high, freq_high;
} napple_vibration;
void napple_input_set_vibration(napple_input *in, uint32_t npad_id, int side /*0 left,1 right*/, const napple_vibration *v);
void napple_input_set_sixaxis_enabled(napple_input *in, int enabled);
/* Npad assignment the guest configured (hid_core.h keeps the master copy and pushes it here whenever it changes). `active` 0 = DeactivateNpad: the npad
   sections stop updating. `disconnected` bit per slot 0..9. `perm[slot]` = which physical pad feeds the slot (SwapNpadAssignment). `dual_of[slot]` != 0:
   the slot reports a Joy-Con pair (MergeSingleJoyAsDualJoy). `keyboard_locks`: bit0 num lock, bit1 caps lock, bit2 scroll lock (SendKeyboardLockKeyEvent),
   shown as the matching HidKeyboardModifier bits. */
typedef struct napple_npad_assign {
    int active;
    uint32_t disconnected;
    uint8_t perm[10], dual_of[10];
    uint32_t keyboard_locks;
} napple_npad_assign;
void napple_input_set_assignment(napple_input *in, const napple_npad_assign *a);
/* SetTouchScreenResolution: the coordinate space touch (and mouse) positions are reported in. */
void napple_input_set_screen_size(napple_input *in, int w, int h);
/* Sections the guest has to activate before the backend writes them (ActivateDebugPad / ActivateGesture / ActivateConsoleSixAxisSensor). */
void napple_input_set_debug_pad_enabled(napple_input *in, int enabled);
void napple_input_set_gesture_enabled(napple_input *in, int enabled);
void napple_input_set_console_sixaxis_enabled(napple_input *in, int enabled);
/* Latest six-axis sample of a player slot (0..9), for IsSixAxisSensorAtRest; returns 0 when the slot has produced none. */
int napple_input_get_sixaxis(napple_input *in, int slot, napple_sixaxis_state *out);
/* Touch screen frame injection (a multi-touch frontend, tests): replaces the mouse-derived finger while set; count 0 releases. */
void napple_input_touch_points(napple_input *in, const napple_touch_point *pts, uint32_t count);
void napple_input_shake(napple_input *in); /* player 1's sensor shakes side to side for 3 whole cycles (0.6 s): games that detect a shake (SMO cap throw) */

/* ---- host events, fed by the frontend (or a test) ---- */
void napple_input_key(napple_input *in, uint16_t mac_keycode, int down); /* macOS virtual keycode */
/* Several keys at the same instant (chords such as L+R): one lock, so the input thread sees them in the same sample. */
void napple_input_keys(napple_input *in, const uint16_t *mac_keycodes, int count, int down);
void napple_input_mouse_position(napple_input *in, float nx, float ny); /* normalised 0..1 inside the game view, y down */
void napple_input_mouse_button(napple_input *in, int button, int down); /* 0 left, 1 right, 2 middle */
void napple_input_mouse_scroll(napple_input *in, float dx, float dy);
/* On-screen controller (iOS touch overlay): the pressed Switch buttons (NAPPLE_BTN_MASK bits A..DOWN) and analog sticks in [-1,1] (y up) for player 1,
   merged with keyboard and controllers (larger stick deflection wins). Ignored while blocked or unfocused; cleared when either starts. Call with zeros to release. */
void napple_input_virtual_pad(napple_input *in, uint64_t buttons, float lx, float ly, float rx, float ry);
void napple_input_focus(napple_input *in, int focused); /* losing focus releases everything */
/* Release all held host keys/buttons (menu opened, etc). */
void napple_input_release_all(napple_input *in);
/* While blocked (overlay menu open, window unfocused) host and controller input reads as neutral. */
void napple_input_set_blocked(napple_input *in, int blocked);

/* ---- hot keys from controllers (Home / Guide, Share / Capture): fired on the press edge from the input thread ---- */
enum { NAPPLE_HOTKEY_HOME = 1, NAPPLE_HOTKEY_CAPTURE = 2 };
typedef void (*napple_hotkey_fn)(void *user, int hotkey);
void napple_input_set_hotkey_callback(napple_input *in, napple_hotkey_fn cb, void *user);
/* Operation mode of the console as configured by the player (bindings.operation_mode): 0 docked, 1 handheld. */
int napple_input_operation_mode(napple_input *in);
/* Last vibration the guest asked for on a player slot (0..9) side (0 left, 1 right); for tests and the UI. Returns 0 when none. */
int napple_input_get_vibration(napple_input *in, int slot, int side, napple_vibration *out);

/* ---- snapshot (UI + tests) ---- */
typedef struct napple_input_snapshot {
    napple_npad_state pad;        /* merged state sent to the guest */
    int controllers;              /* GameControllers connected */
    char controller_name[64];     /* first controller, or "" */
    char controller_names[8][64]; /* every connected controller, in player order (player 1 first) */
    uint32_t player_style[8];     /* style reported to the guest per player slot (0 = slot not in use) */
    int touch_active;
    uint32_t touch_x, touch_y;
} napple_input_snapshot;
void napple_input_get_snapshot(napple_input *in, napple_input_snapshot *out);

/* ---- bindings ---- */
#define NAPPLE_KEY_NONE 0xFFFF
typedef struct napple_bindings {
    uint16_t key[NAPPLE_BTN_COUNT_BASIC][2]; /* up to two mac keycodes per Switch button A..DOWN (NAPPLE_KEY_NONE = unbound) */
    uint16_t stick_l[4], stick_r[4];         /* left, up, right, down keys of each stick */
    int16_t pad[NAPPLE_BTN_COUNT_BASIC][2];  /* up to two physical elements (NAPPLE_PAD_*) per Switch button; -1 none */
    float stick_deadzone;                    /* 0..0.9 radial, default 0.12 */
    int mouse_touch;                         /* 1: left mouse button = touch screen */
    int face_layout;                         /* 0 auto (by position, except Nintendo pads), 1 by position, 2 by label */
    /* analog tuning (applies to controller sticks; invert also to the keyboard sticks) */
    float stick_range;       /* raw deflection that counts as full (0.5..1, default 1): lower it for worn sticks */
    float stick_curve;       /* response exponent after the dead zone (0.5..3, default 1 = linear; >1 = finer near the centre) */
    float trigger_threshold; /* ZL/ZR press point on analog triggers (0.05..0.95, default 0.5) */
    uint8_t invert[4];       /* left X, left Y, right X, right Y (1 = inverted) */
    int style;               /* npad style reported: 0 auto, 1 Pro Controller, 2 Joy-Con pair, 3 handheld (when the game supports it) */
    int operation_mode;      /* 0 docked (1080p, TV), 1 handheld (720p); read by the runtime at start-up */
} napple_bindings;
enum { NAPPLE_STYLE_PREF_AUTO = 0, NAPPLE_STYLE_PREF_PRO = 1, NAPPLE_STYLE_PREF_JOYCON = 2, NAPPLE_STYLE_PREF_HANDHELD = 3 };
/* Physical controller elements (position based, so one binding means the same place on every pad). */
enum {
    NAPPLE_PAD_SOUTH = 0,
    NAPPLE_PAD_EAST,
    NAPPLE_PAD_WEST,
    NAPPLE_PAD_NORTH,
    NAPPLE_PAD_L3,
    NAPPLE_PAD_R3,
    NAPPLE_PAD_LB,
    NAPPLE_PAD_RB,
    NAPPLE_PAD_LT,
    NAPPLE_PAD_RT,
    NAPPLE_PAD_MENU,
    NAPPLE_PAD_OPTIONS,
    NAPPLE_PAD_DLEFT,
    NAPPLE_PAD_DUP,
    NAPPLE_PAD_DRIGHT,
    NAPPLE_PAD_DDOWN,
    NAPPLE_PAD_COUNT
};
void napple_bindings_default(napple_bindings *b);
int napple_input_get_bindings(napple_input *in, napple_bindings *out);
int napple_input_set_bindings(napple_input *in, const napple_bindings *b); /* validates, applies and saves */
int napple_input_reset_bindings(napple_input *in);
/* Per-player profiles (players 1..8 = slot 0..7): a player with a profile maps keyboard / controller input with it instead of the shared bindings
   (input-p<N>.json next to input.json). get returns 1 when the slot has its own profile, 0 when it follows the shared bindings (out is then the shared set),
   -1 for a bad slot. set validates, applies and saves; clear removes the profile. */
int napple_input_get_player_bindings(napple_input *in, int slot, napple_bindings *out);
int napple_input_set_player_bindings(napple_input *in, int slot, const napple_bindings *b);
int napple_input_clear_player_bindings(napple_input *in, int slot);
/* Latest state sent to player slot 0..7 (for the live controller display). Returns 1 when the slot is in use, 0 when not (out is then the last known state). */
int napple_input_get_pad_state(napple_input *in, int slot, napple_npad_state *out);
/* JSON (pure; used by the above and unit tests). write returns bytes needed (snprintf style). */
size_t napple_bindings_to_json(const napple_bindings *b, char *out, size_t cap);
int napple_bindings_from_json(const char *json, size_t len, napple_bindings *out); /* 0 ok, <0 error; out untouched on error */
/* Binding capture for the remap UI: the next key press / controller press is delivered to cb, once. */
typedef void (*napple_capture_fn)(void *user, int is_key, int code);
void napple_input_capture_next(napple_input *in, napple_capture_fn cb, void *user);
int napple_input_capture_pending(napple_input *in); /* 1 while a capture is armed (Esc then cancels the capture instead of closing the menu) */
const char *napple_button_name(int button);         /* "A", "ZL", ... */
const char *napple_pad_element_name(int element);
const char *napple_keycode_name(uint16_t mac_keycode);

#ifdef __cplusplus
}
#endif
#endif
