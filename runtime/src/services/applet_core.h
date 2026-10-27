/* applet core: the application-visible settings the guest sets through ISelfController / IAudioController / IApplicationFunctions and
 * reads back (portable). Before this the setters were no-ops and the getters constants; now a set value is what a get returns. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { APPLET_SCREENSHOT_INHERIT = 0, APPLET_SCREENSHOT_ENABLE = 1, APPLET_SCREENSHOT_DISABLE = 2 };

typedef struct {
    float main_volume, library_volume, transparent_volume_rate; /* IAudioController: expected master volumes, 0..1 */
    int vr_mode, auto_sleep_disabled, restart_message, capture_short_press_message, album_image_taken_notification;
    int screenshot_permission; /* APPLET_SCREENSHOT_* */
    uint32_t idle_detection_extension, input_detection_source_set, screenshot_orientation, keyboard_layout, wireless_priority_mode;
    uint32_t cpu_boost_mode;
    unsigned cpu_boost_requests;
    uint32_t game_play_recording_state, media_playback_state;
    int controller_firmware_update_section, crash_report_enabled, copyright_visible;
    unsigned home_button_block, home_button_short_long_block; /* nesting depth of BeginBlocking... */
    uint32_t user_active_reports;
    uint32_t operation_mode_notification, performance_mode_notification, focus_handling_mode;
    /* sleep: Allow/DisallowToEnterSleep is a flag; the lock is a counted acquire (Request/Release); Transient drops it until the next Request */
    int sleep_disallowed, sleep_lock_held, sleep_lock_transient;
    unsigned sleep_lock_acquires;
    unsigned fatal_section; /* nesting depth of Enter/LeaveFatalSection */
    int window_visible, foreground_rights, reject_background, handles_request_to_display;
    unsigned approve_to_display_count;
    uint64_t gpu_time_slice; /* SetAppletGpuTimeSlice (ns); 0 = unset */
    int32_t auto_sleep_override[4];
    int auto_sleep_overridden;
    uint32_t last_multimedia_error;
    unsigned multimedia_error_reports;
    uint8_t screenshot_identity[0x18];
    int screenshot_identity_set;
    uint64_t album_user_data_size, album_user_data_hash;
    uint64_t suspended_ticks;           /* AccumulatedSuspendedTickValue: time the application spent suspended (never, while it runs in focus) */
    int request_exit_to_library_applet; /* SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled */
    int game_play_recording_initialized;
    unsigned game_play_recording_flushes;
    int copyright_frame_buffer_initialized;
    uint64_t copyright_image_size;
    uint32_t platform_region; /* 1 Global, 2 China */
    int illuminance_available;
    float illuminance; /* lux */
} AppletState;

void applet_state_init(AppletState *s);
float applet_clamp_volume(float v, float fallback); /* NaN -> fallback, clamped to 0..1 */
int applet_valid_screenshot_permission(uint32_t v);
/* Begin/End nesting: End never goes below 0. Returns whether blocking is active afterwards. */
int applet_block_begin(unsigned *depth);
int applet_block_end(unsigned *depth);
/* Screenshots are allowed unless the guest disabled them. */
int applet_screenshot_allowed(const AppletState *s);

/* ---- sleep / illuminance / sleep-lock helpers -------------------------------------------------------------------------------------------- */
/* RequestToAcquireSleepLock: the lock is granted at once (nothing else contends for it); returns whether the acquired event must be signalled. */
int applet_sleep_lock_acquire(AppletState *s);
void applet_sleep_lock_release(AppletState *s);
void applet_sleep_lock_release_transiently(AppletState *s);
/* Enter/LeaveFatalSection nest like the home-button block (End below zero is ignored). */
int applet_fatal_enter(AppletState *s);
int applet_fatal_leave(AppletState *s);
/* The illuminance sensor: absent unless a host value is supplied (NAPPLE_ILLUMINANCE=<lux>). Reading while absent gives the nominal 100 lux the previous
 * answer used. `over_limit` is the Ex variant's flag: the value is above 100000 lux (clamped to it). */
void applet_illuminance_set(AppletState *s, int available, float lux);
float applet_illuminance_get(const AppletState *s, int *over_limit);

/* ---- capture slots (IDisplayController) -------------------------------------------------------------------------------------------------- */
/* The display controller keeps up to three 1280x720 RGBA8 images: the last foreground image, the last application image and the caller applet's.
 * They are filled from the host's last presented frame (a source the frontend registers; none = the slots stay empty and read as opaque black). */
enum { APPLET_CAP_LAST_FOREGROUND = 0, APPLET_CAP_LAST_APPLICATION = 1, APPLET_CAP_CALLER_APPLET = 2, APPLET_CAP_SLOTS = 3 };
#define APPLET_CAP_W 1280u
#define APPLET_CAP_H 720u
#define APPLET_CAP_BYTES (APPLET_CAP_W * APPLET_CAP_H * 4u)
/* Source: copies the last presented frame into `rgba` (w*h*4 bytes, row stride w*4) and returns 1, or returns 0 when there is no frame yet. */
typedef int (*AppletFrameSource)(uint8_t *rgba, uint32_t w, uint32_t h);
void applet_capture_set_source(AppletFrameSource src); /* NULL (the default) = none */
/* Capture the source's frame into a slot; returns 1 when a frame was copied, 0 when the slot now holds black. */
int applet_capture_update(unsigned slot);
/* Read a slot into out (cap bytes, up to APPLET_CAP_BYTES); returns the bytes written (the slot is black until updated), 0 for a bad slot. */
size_t applet_capture_read(unsigned slot, uint8_t *out, size_t cap, int *has_frame);
int applet_capture_copy(unsigned dst, unsigned src); /* 0 bad slot */
int applet_capture_has_frame(unsigned slot);
/* The last application capture buffer can be held by one user at a time (Acquire/ReleaseLastApplicationCaptureBuffer): returns 0 when it is already held. */
int applet_capture_acquire_app_buffer(void);
void applet_capture_release_app_buffer(void);
void applet_capture_reset(void);

/* ---- host events (applet.c): wake the system events the guest holds; hooks for the frontend and tests ---------------------------------------- */
void applet_signal_gpu_error(void);           /* IApplicationFunctions::GetGpuErrorDetectedSystemEvent */
void applet_signal_friend_invitation(void);   /* GetFriendInvitationStorageChannelEvent */
void applet_display_resolution_changed(void); /* GetDefaultDisplayResolutionChangeEvent (also raised by an operation-mode message) */
int applet_sleep_disallowed(void);            /* the guest asked the host not to sleep (Disallow, a held sleep lock, or auto sleep disabled) */
