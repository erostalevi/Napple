/* settings core (portable): region, time zone name and colour set of set / set:sys, derived from the host language and configuration. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { SET_REGION_JPN = 0, SET_REGION_USA = 1, SET_REGION_EUR = 2, SET_REGION_AUS = 3, SET_REGION_HTK = 4, SET_REGION_CHN = 5 };
enum { SET_COLOR_WHITE = 0, SET_COLOR_BLACK = 1 };
#define SET_TZ_NAME_SIZE 0x24u

/* Region of a console that would ship with `lang` (8 ASCII bytes of the Switch language code) as its desired language.
 * `env` (NAPPLE_REGION: "jpn" "usa" "eur" "aus" "htk" "chn", or a number 0..5) overrides it when valid. */
uint32_t settings_region(uint64_t lang, const char *env);
/* Fills the 0x24-byte LocationName: `env` (NAPPLE_TIMEZONE) when it is a plausible "Area/City" or "UTC" style name, else "UTC". */
void settings_timezone_name(const char *env, char out[SET_TZ_NAME_SIZE]);
/* Colour set id: NAPPLE_COLOR_SET "white"/"black"/"0"/"1"; default black (matches the HOME menu theme the SDK expects by default). */
uint32_t settings_color_set(const char *env);

/* Key code map (GetKeyCodeMap, 2 and 3): the US keyboard layout the on-screen/physical keyboard translation reads. The reply buffer is SET_KEYMAP_SIZE bytes: one 16-byte
 * entry per HID keyboard usage id (0..0xff) = {u16 unshifted, u16 shifted, u16 alt-gr, u16 shift+alt-gr, u32 flags, u32 reserved}, the u16s being UTF-16 code units (0 = no
 * character), flags bit 0 = the usage produces a character. Layout and entry shape are the author's reading of the public description (unverified on a console).
 * A buffer smaller than the map receives its prefix; the function returns the number of bytes written. */
#define SET_KEYMAP_SIZE 0x1000u
#define SET_KEYMAP_ENTRY 16u
size_t settings_keycode_map(uint8_t *out, size_t cap);
