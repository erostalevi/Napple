/* hid core: per-session settings the hid service stores and returns (portable; no input library, no Mach). The shared-memory input state
 * itself is owned by input/; this is the part that used to be a constant: six-axis fusion and zero-drift mode per sensor handle, joy hold
 * type, handheld activation mode, home-button protection, vibration permission, communication mode, supported styles. */
#pragma once
#include <stdint.h>

#define HIDC_MAX_SENSORS 16
#define HIDC_MAX_HOME_IDS 16
#define HIDC_MAX_CAPTURE 8
#define HIDC_MAX_VIB_ACTIVE 16
typedef struct {
    uint32_t handle;
    int used, fusion_enabled;
    float gain, reduction;
    uint32_t zero_drift;
} HidcSensor;
typedef struct {
    HidcSensor sensors[HIDC_MAX_SENSORS];
    uint32_t styles;
    int styles_set;
    int64_t joy_hold_type, handheld_mode;
    uint32_t home_ids[HIDC_MAX_HOME_IDS];
    int home_on[HIDC_MAX_HOME_IDS];
    unsigned nhome;
    int vibration_permitted;
    uint64_t comm_mode;
    /* npad assignment (DeactivateNpad, DisconnectNpad, SwapNpadAssignment, MergeSingleJoyAsDualJoy, LR assignment mode) */
    int npad_active;       /* ActivateNpad / DeactivateNpad */
    uint32_t disconnected; /* bit per slot 0..9: DisconnectNpad until the guest re-arms the pad */
    uint8_t perm[10];      /* source pad of each slot (identity until SwapNpadAssignment) */
    uint8_t dual_of[10];   /* slot a: 1 + the slot merged into it by MergeSingleJoyAsDualJoy, 0 = none */
    int lr_assign_mode;    /* StartLrAssignmentMode .. StopLrAssignmentMode */
    int center_clamp;      /* SetNpadAnalogStickUseCenterClamp */
    struct {
        uint32_t style;
        int button; /* HidNpadButton bit index the capture button maps to, -1 none */
    } capture[HIDC_MAX_CAPTURE];
    unsigned ncapture;
    uint32_t kb_locks;         /* bit0 num lock, bit1 caps lock, bit2 scroll lock */
    uint32_t touch_mode;       /* SetTouchScreenConfiguration */
    uint32_t touch_w, touch_h; /* SetTouchScreenResolution; 0 = the default 1280x720 */
    int console_six_active, console_six_started;
    int gesture_active, debug_pad_active;
    uint32_t vib_active[HIDC_MAX_VIB_ACTIVE];
    unsigned nvib_active;
} HidCore;

enum { HIDC_ZERO_DRIFT_LOOSE = 0, HIDC_ZERO_DRIFT_STANDARD = 1, HIDC_ZERO_DRIFT_TIGHT = 2 };
#define HIDC_DEFAULT_GAIN 0.03f
#define HIDC_DEFAULT_REDUCTION 0.4f

void hidc_init(HidCore *h);
HidcSensor *hidc_sensor(HidCore *h, uint32_t handle);  /* created with defaults on first use; NULL if the table is full */
int hidc_set_zero_drift(HidcSensor *s, uint32_t mode); /* 0, or -1 for a mode outside Loose..Tight */
void hidc_reset_fusion(HidcSensor *s);
uint64_t hidc_led_pattern(uint32_t npad_id); /* player LED bit pattern for an NpadIdType; 0 for handheld/other/unknown */
void hidc_home_protection_set(HidCore *h, uint32_t npad_id, int on);
int hidc_home_protection_get(const HidCore *h, uint32_t npad_id);

/* ---- npad assignment ---- */
int hidc_slot_from_id(uint32_t npad_id);           /* NpadIdType -> slot: players 1..8 = 0..7, "other" (0x10) = 8, handheld (0x20) = 9; -1 for anything else */
int hidc_disconnect(HidCore *h, uint32_t npad_id); /* 0, or -1 for an unknown id */
void hidc_reconnect_all(HidCore *h);               /* the guest re-armed the pads (SetSupportedNpadIdType / ActivateNpad) */
int hidc_swap(HidCore *h, uint32_t id_a, uint32_t id_b);  /* players 1..8 only: 0, -1 unknown/unsupported id, -2 same pad */
int hidc_merge(HidCore *h, uint32_t id_a, uint32_t id_b); /* b joins a as one dual Joy-Con pair: 0, -1 unknown id, -2 same pad. b is disconnected */
int hidc_pad_source(const HidCore *h, int slot);          /* the pad that feeds `slot` (after swaps) */
/* ---- keyboard lock keys: HidKeyboardLockKeyEvent bits: per key (num, caps, scroll) On = 1<<(3k), Off = 1<<(3k+1), Toggle = 1<<(3k+2) ---- */
uint32_t hidc_keyboard_lock_event(HidCore *h, uint32_t events); /* returns the lock state: bit0 num, bit1 caps, bit2 scroll */
/* ---- capture button assignment (per single npad style bit) ---- */
int hidc_capture_assign(HidCore *h, uint32_t style, int button); /* 0, -1 when `style` is not exactly one style bit or the table is full */
int hidc_capture_clear(HidCore *h, uint32_t style);              /* 0, -1 invalid style */
int hidc_capture_get(const HidCore *h, uint32_t style);          /* the assigned button or -1 */
/* ---- touch screen ---- */
int hidc_set_touch_resolution(HidCore *h, uint32_t w, uint32_t hgt); /* 0, or -1 when w or h is 0 or above 4096 */
/* ---- vibration device handles: HidVibrationDeviceHandle bytes {type, npad style index, player number, device index} ---- */
int hidc_vib_handle_decode(uint32_t handle, uint32_t *npad_id, uint32_t *side); /* 0 valid, -1 invalid; handheld handles map to npad id 0x20 */
int hidc_vib_activate(HidCore *h, uint32_t handle);                             /* 0, -1 invalid handle, -2 list full; re-activating is fine */
int hidc_vib_is_active(const HidCore *h, uint32_t handle);
