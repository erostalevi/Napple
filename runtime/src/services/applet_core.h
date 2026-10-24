/* applet core: the application-visible settings the guest sets through ISelfController / IAudioController / IApplicationFunctions and
 * reads back (portable). Before this the setters were no-ops and the getters constants; now a set value is what a get returns. */
#pragma once
#include <stdint.h>

enum { APPLET_SCREENSHOT_INHERIT = 0, APPLET_SCREENSHOT_ENABLE = 1, APPLET_SCREENSHOT_DISABLE = 2 };

typedef struct {
    float main_volume, library_volume, transparent_volume_rate;     /* IAudioController: expected master volumes, 0..1 */
    int vr_mode, auto_sleep_disabled, restart_message, capture_short_press_message, album_image_taken_notification;
    int screenshot_permission;                                       /* APPLET_SCREENSHOT_* */
    uint32_t idle_detection_extension, input_detection_source_set, screenshot_orientation, keyboard_layout, wireless_priority_mode;
    uint32_t cpu_boost_mode; unsigned cpu_boost_requests;
    uint32_t game_play_recording_state, media_playback_state;
    int controller_firmware_update_section, crash_report_enabled, copyright_visible;
    unsigned home_button_block, home_button_short_long_block;        /* nesting depth of BeginBlocking... */
    uint32_t user_active_reports;
    uint32_t operation_mode_notification, performance_mode_notification, focus_handling_mode;
} AppletState;

void applet_state_init(AppletState *s);
float applet_clamp_volume(float v, float fallback);                 /* NaN -> fallback, clamped to 0..1 */
int applet_valid_screenshot_permission(uint32_t v);
/* Begin/End nesting: End never goes below 0. Returns whether blocking is active afterwards. */
int applet_block_begin(unsigned *depth);
int applet_block_end(unsigned *depth);
/* Screenshots are allowed unless the guest disabled them. */
int applet_screenshot_allowed(const AppletState *s);
