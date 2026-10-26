/* Library applet answers (portable): how the headless host answers the error (0xe), player select (0x10) and controller support (0xc)
 * library applets that a game launches through ILibraryAppletCreator. There is no screen to show them on, so each one is answered the way the
 * applet answers when the user confirms with the only profile / the only pad: the game continues instead of the process dying.
 * Layouts follow libnx's public structs as remembered and are [unverified] against a title; see docs/architecture/runtime-services.md. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { LIBAPPLET_ID_CONTROLLER = 0x0c, LIBAPPLET_ID_ERROR = 0x0e, LIBAPPLET_ID_PLAYER_SELECT = 0x10, LIBAPPLET_ID_SWKBD = 0x11 };

/* Error applet. The guest pushes CommonArguments, then ErrorCommonHeader { u8 type; u8 jump_flag; ... } and an argument storage. It reads no
 * output: the applet terminates after the user dismisses it. Answer: dismissed (success), never jump to system settings. */
typedef struct {
    int valid;
    uint8_t type, jump_flag;
    uint32_t first_word, second_word;
    int has_word, has_word2;
} AppletErrorInfo;
void applet_error_parse(const uint8_t *header, size_t header_size, const uint8_t *arg, size_t arg_size, AppletErrorInfo *out);
/* One log line describing the request (the error code a game hits is the most useful thing a headless run can report). */
void applet_error_describe(const AppletErrorInfo *e, char *buf, size_t cap);

/* Error code text for the dialog: type 1 (error code) shows the first two argument words as "CCCC-NNNN"; any other type shows the first word as a Result
 * ("2MMM-DDDD": module = r & 0x1ff, description = r >> 9 & 0x1fff). [unverified against a title] Empty when the arguments are missing. */
void applet_error_code_text(const AppletErrorInfo *e, char *buf, size_t cap);

/* Software keyboard answer. Config (KeyboardConfig 0x4C8): +0 type (0 normal, 1 numpad, 2 qwerty), +0x3AC max, +0x3B0 min, +0x3B4 password. */
typedef struct {
    uint32_t type, min_len, max_len;
    int password, numeric;
} AppletSwkbdConfig;
#define APPLET_SWKBD_MAX_LEN 500u
void applet_swkbd_parse(const uint8_t *cfg /* may be NULL */, size_t size, AppletSwkbdConfig *c);
/* Fit a typed text to the config: truncates to max_len characters (UTF-8 code points; 0 = no limit), returns 0 when the result has fewer than min_len
 * characters (the applet's OK button stays disabled, so a headless answer is a cancel), else 1. */
int applet_swkbd_fit(const AppletSwkbdConfig *c, const char *text, char *out, size_t cap);

/* Player select (profile selector). Output storage: PlayerSelectResultInfo { u64 result; u8 uid[16] } (0x18). result 0 = success. */
enum { APPLET_PLAYER_SELECT_OK = 0, APPLET_PLAYER_SELECT_CANCELLED = 2 };
#define APPLET_PLAYER_SELECT_OUT_SIZE 0x18u
/* Choose among `count` profiles: index is the wanted one (out of range -> 0); count 0 or cancel -> cancelled. Returns the uid pointer chosen or NULL. */
const uint8_t *applet_player_select_pick(const uint8_t (*uids)[16], unsigned count, int index, int cancel);
void applet_player_select_answer(const uint8_t user_id[16], int cancel, uint8_t out[APPLET_PLAYER_SELECT_OUT_SIZE]);

/* Controller support. Arg header: s8 player_count_min, s8 player_count_max, u8 take_over_connection, u8 left_justify, u8 permit_joy_dual,
 * u8 single_mode, u8 identification_color (counts <= 0 mean unset). Output ControllerSupportResultInfo { s32 players; u32 selected_id; u32 result }
 * (0xC; the older 8-byte layout reads the first two fields). One virtual pad: players = min..max clamped to 1.. (the game asked for at
 * least 1 and at most its maximum). `connected` is the number of pads the input backend reports (keyboard-only counts as 1): players = connected
 * clamped to min..max (short_of_min is set when fewer are connected than asked: the real applet would keep waiting); single mode reports npad 0 (Player 1) as the selected controller, otherwise 0xffffffff. */
typedef struct {
    int32_t players;
    uint32_t min, max;
    int single_mode;
    uint32_t selected_id;
    int connected, short_of_min;
} AppletControllerAnswer;
#define APPLET_CONTROLLER_OUT_SIZE 0xCu
void applet_controller_answer(const uint8_t *arg, size_t arg_size, int connected, AppletControllerAnswer *a);
void applet_controller_encode(const AppletControllerAnswer *a, uint8_t out[APPLET_CONTROLLER_OUT_SIZE]);
