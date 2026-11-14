/* Pure helpers behind the in-game overlay (no AppKit): unit tested by tests/test_overlay_logic.c. */
#ifndef NAPPLE_OVERLAY_LOGIC_H
#define NAPPLE_OVERLAY_LOGIC_H
#include <stddef.h>
#include "napple_frontend.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FE_TAB_COUNT 5 /* 0 Video, 1 Speed, 2 Controls, 3 Game, 4 Status */

/* cur + delta wrapped into 0..count-1 (count <= 0 gives 0). */
int fe_tab_wrap(int cur, int delta, int count);
/* "50%", "100% (native)", "2x supersampled" style labels. */
void fe_format_scale(int percent, char *out, size_t cap);
/* "1920 x 1080 (1080p)"; the class is omitted for sizes that are not a standard 16:9 step. */
void fe_format_resolution(int w, int h, char *out, size_t cap);
/* "1x", "0.5x", "2.5x" from a percent value. */
void fe_format_speed(int percent, char *out, size_t cap);
/* Parse a user typed integer (spaces and a trailing % allowed) into lo..hi; returns fallback when the text is not a number. */
int fe_parse_percent(const char *text, int lo, int hi, int fallback);
/* DLC switches (settings.disabled_dlc is the list of title ids switched off; ids compare case-insensitively). */
int fe_dlc_is_disabled(const napple_settings *s, const char *title_id);
/* Returns 1 when the settings changed. Refuses (0) to switch off a 33rd id. */
int fe_dlc_set_enabled(napple_settings *s, const char *title_id, int enabled);
/* Update version choice: NULL or "" = latest installed. */
void fe_update_choose(napple_settings *s, const char *version);
/* Back to the defaults for everything the overlay's Video and Speed tabs and the statistics HUD edit; audio and the game's
   update / DLC choices are kept. */
void fe_reset_display_speed(napple_settings *s);

#ifdef __cplusplus
}
#endif
#endif
