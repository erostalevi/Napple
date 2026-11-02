/* Library applet answers (portable): how the headless host answers the error (0xe), player select (0x10) and controller support (0xc)
 * library applets that a game launches through ILibraryAppletCreator. There is no screen to show them on, so each one is answered the way the
 * applet answers when the user confirms with the only profile / the only pad: the game continues instead of the process dying.
 * Layouts follow libnx's public structs as remembered and are [unverified] against a title; see docs/architecture/runtime-services.md. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { LIBAPPLET_ID_CONTROLLER = 0x0c, LIBAPPLET_ID_ERROR = 0x0e, LIBAPPLET_ID_PLAYER_SELECT = 0x10, LIBAPPLET_ID_SWKBD = 0x11 };

/* Error applet. The guest pushes CommonArguments, then the error viewer's startup parameter (one storage; for some modes an extra storage follows:
 * the ErrorContext (0x200), the ResultBacktrace (0x84) or the EULA data). Layout from MK8D v1376256's sdk (nn::err, +0x11a51c..+0x11b2e4):
 *   +0 u8 type, +1 u8 jump flag (1: the applet itself may jump to System Settings; 0: ShowErrorWithoutJump), +5 u8 a ResultBacktrace follows,
 *   +6 u8 the argument is an ErrorCode (else a Result), +7 u8 an ErrorContext follows.
 *   type 0 normal (0x14):        ErrorCode {u32 category, u32 number} at +8 when +6 is set, else the Result at +0x10
 *   type 1 system error (0x1018): ErrorCode at +8, language code at +0x10, dialog message (UTF-8, 0x800) at +0x18, full-screen message at +0x818
 *   type 2 application error (0x1014): application error number u32 at +8, language code at +0xc, dialog message at +0x14, full-screen at +0x814
 *   type 3 EULA / 8 system update EULA (0xc): region code u32 at +8 (type 8 pushes the EULA data, 0x20000, as a second storage)
 *   type 4 (0xc): a Result at +8 (ShowError for module 142 descriptions 100..119)
 *   type 5 error record (0x18): ErrorCode at +8 (from a Result: category 2000 + module, number = description), posix time at +0x10
 *   type 6 recorded error (CreateErrorViewerStartupParamForRecordedError): no code read here
 * Output: the SDK waits for the applet (nn::err +0x11b1c8: Start, Join, GetLibraryAppletExitReason), then TryPopFromOutChannel: when a storage is
 * there it reads 2 bytes and returns byte 1, the jump destination that only ShowErrorWithoutJump hands back to the caller; when the channel is
 * empty (PopOutData answers NoDataInChannel, 0x480) it returns 0, no jump. The headless answer is "dismissed, no jump": exit normally, no output. */
enum {
    APPLET_ERROR_TYPE_NORMAL = 0, APPLET_ERROR_TYPE_SYSTEM = 1, APPLET_ERROR_TYPE_APPLICATION = 2, APPLET_ERROR_TYPE_EULA = 3, APPLET_ERROR_TYPE_RESULT = 4,
    APPLET_ERROR_TYPE_RECORD = 5, APPLET_ERROR_TYPE_RECORDED = 6, APPLET_ERROR_TYPE_SYSTEM_UPDATE_EULA = 8
};
typedef struct {
    int valid;
    uint8_t type, jump_flag;
    uint32_t first_word, second_word; /* the code: ErrorCode category / number, the Result (first only), or the application error number */
    int has_word, has_word2;
    int is_result, is_app_code;       /* first_word is a Result / an application error number */
    int has_context, has_backtrace;   /* an ErrorContext / ResultBacktrace storage follows */
    uint32_t region;                  /* EULA types */
    char message[256];                /* system / application error: the dialog message (UTF-8), empty otherwise */
} AppletErrorInfo;
/* `header` is the startup parameter storage. `arg` is the storage after it, or NULL: a startup parameter shorter than its type's layout (an older
 * caller) takes the code from it (an ErrorCode, two words, for a system error; else a Result, one word). */
void applet_error_parse(const uint8_t *header, size_t header_size, const uint8_t *arg, size_t arg_size, AppletErrorInfo *out);
/* One log line describing the request (the error code a game hits is the most useful thing a headless run can report). */
void applet_error_describe(const AppletErrorInfo *e, char *buf, size_t cap);
/* Name of an error type ("normal", "system", ...; "type N" for others). */
const char *applet_error_type_name(uint8_t type);

/* Error code text for the dialog: an ErrorCode as "CCCC-NNNN", a Result as "2MMM-DDDD" (module = r & 0x1ff, description = r >> 9 & 0x1fff),
 * an application error number as "%04u". Empty when the parameter carries no code. */
void applet_error_code_text(const AppletErrorInfo *e, char *buf, size_t cap);

/* Software keyboard answer. Config (KeyboardConfig 0x4C8): +0 type (0 normal, 1 numpad, 2 qwerty), +0x3AC max, +0x3B0 min, +0x3B4 password. */
typedef struct {
    uint32_t type, min_len, max_len;
    int password, numeric;
    char header[400], sub[800], guide[800]; /* UTF-8 of the config's header (+0x24), sub (+0xA6) and guide (+0x1A8) texts; empty when absent */
} AppletSwkbdConfig;
/* UTF-16LE (at most max_units units, stops at a NUL) to UTF-8 with surrogate pairs; always NUL-terminated. */
void applet_utf16le_to_utf8(const uint8_t *p, unsigned max_units, char *out, size_t cap);
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
typedef struct AppletControllerAnswer {
    int32_t players;
    uint32_t min, max;
    int single_mode;
    uint32_t selected_id;
    int connected, short_of_min;
} AppletControllerAnswer;
#define APPLET_CONTROLLER_OUT_SIZE 0xCu
void applet_controller_answer(const uint8_t *arg, size_t arg_size, int connected, AppletControllerAnswer *a);
void applet_controller_encode(const AppletControllerAnswer *a, uint8_t out[APPLET_CONTROLLER_OUT_SIZE]);

/* Handler registry: another area (applets-rare, a frontend) can answer a library applet id without editing applet.c. A registered handler runs in
 * Start before the built-in answers; it returns 1 when it answered (set the output with libapplet_call_set_out, the GetResult code in result) and 0
 * to fall through to the built-in answer / firmware host. At most one handler per id; registering again replaces it, NULL removes it. */
#define LIBAPPLET_MAX_IN 8
typedef struct {
    const uint8_t *data; /* NULL for a transfer-memory storage */
    size_t size;
    int transfer_memory;
} LibAppletIn;
typedef struct {
    uint32_t applet_id, mode;
    unsigned nin;
    LibAppletIn in[LIBAPPLET_MAX_IN];
    uint8_t *out; /* malloc'd output storage (NULL = no output); owned by the caller after the handler returns */
    size_t out_size;
    uint32_t result; /* GetResult code; 0 = success */
    int unsupported; /* handler set: report the applet as terminated with `result` */
    /* interactive channel: the storages the caller pushed with PushInteractiveInData, and the ones a handler answers with (popped in order by
     * PopInteractiveOutData). Output storages are malloc'd by libapplet_call_push_interactive_out. */
    unsigned ninteractive_in;
    LibAppletIn interactive_in[LIBAPPLET_MAX_IN];
    unsigned ninteractive_out;
    uint8_t *interactive_out[LIBAPPLET_MAX_IN];
    size_t interactive_out_size[LIBAPPLET_MAX_IN];
} LibAppletCall;
typedef int (*LibAppletHandler)(LibAppletCall *call);
int libapplet_register_handler(uint32_t id, LibAppletHandler handler); /* 0 ok, -1 table full */
LibAppletHandler libapplet_find_handler(uint32_t id);
void libapplet_clear_handlers(void);
/* Run the handler of call->applet_id if any; returns whether it answered. */
int libapplet_dispatch(LibAppletCall *call);
/* Copies `size` bytes (zero-filled when data is NULL) into a fresh output storage of the call. */
void libapplet_call_set_out(LibAppletCall *call, const void *data, size_t size);
int libapplet_call_push_interactive_out(LibAppletCall *call, const void *data, size_t size); /* 0 ok, -1 queue full */
void libapplet_call_free(LibAppletCall *call);                                               /* frees out and the interactive outputs */

/* ---- the firmware applet table ------------------------------------------------------------------------------------------------------------- */
/* Library applet id -> the program id of the applet in the user's firmware (switchbrew "System Applets" list, remembered, [unverified] against a firmware dump;
 * the Mii Maker's 0x12 -> 0100000000001009 is verified). A firmware applet runs through the host (child process or in-process) when it is enabled for its id
 * and was built beside the game; otherwise the built-in answer (HLE) is used. */
typedef struct {
    uint32_t applet_id;
    const char *program_id;
    const char *name;
    int default_firmware; /* run from firmware by default when present (only the Mii Maker: its HLE answer cannot show an editor) */
} LibAppletProgram;
const LibAppletProgram *libapplet_program_for(uint32_t applet_id); /* NULL when the id has no firmware program */
/* Policy: NAPPLE_FIRMWARE_APPLETS=0 turns every firmware applet off; =all turns all table entries on; otherwise the defaults hold. NAPPLE_FIRMWARE_APPLET_IDS is
 * a comma list ("0x11,0x0c" or names "swkbd,controller") of ids that are also on. `master` and `ids` are the two environment values (NULL when unset). */
int libapplet_firmware_enabled(const LibAppletProgram *p, const char *master, const char *ids);
/* Mode of CreateLibraryApplet: 0 AllForeground, 1 Background, 2 NoUi, 3 BackgroundIndirect, 4 AllForegroundInitiallyHidden. */
int libapplet_mode_is_inline(uint32_t mode); /* swkbd inline keyboard: any non-foreground mode */

/* ---- software keyboard: inline (interactive) mode ------------------------------------------------------------------------------------------- */
/* The game starts the keyboard in a background mode and drives it through the interactive channel: requests {u32 command; payload} in, replies
 * {u32 reply type; payload} out. Commands/replies are from libnx's swkbd as remembered [unverified against a title]. */
enum {
    APPLET_SWKBD_CMD_FINALIZE = 0x4,
    APPLET_SWKBD_CMD_SET_USER_WORD = 0x6,
    APPLET_SWKBD_CMD_SET_CUSTOMIZE_DIC = 0x7,
    APPLET_SWKBD_CMD_CALC = 0xA,
    APPLET_SWKBD_CMD_SET_CUSTOMIZED_DICTIONARIES = 0xB,
    APPLET_SWKBD_CMD_UNSET_CUSTOMIZED_DICTIONARIES = 0xC
};
enum {
    APPLET_SWKBD_REPLY_FINISHED_INITIALIZE = 0x0,
    APPLET_SWKBD_REPLY_DEFAULT = 0x1,
    APPLET_SWKBD_REPLY_CHANGED_STRING = 0x2,
    APPLET_SWKBD_REPLY_MOVED_CURSOR = 0x3,
    APPLET_SWKBD_REPLY_DECIDED_ENTER = 0x5,
    APPLET_SWKBD_REPLY_DECIDED_CANCEL = 0x6,
    APPLET_SWKBD_REPLY_CHANGED_STRING_UTF8 = 0x7,
    APPLET_SWKBD_REPLY_DECIDED_ENTER_UTF8 = 0x9,
    APPLET_SWKBD_REPLY_UNSET_CUSTOMIZED = 0xC
};
#define APPLET_SWKBD_INLINE_REPLY_SIZE 0x400u
typedef struct {
    int initialized, finalized, shown;
    unsigned requests;
    AppletSwkbdConfig cfg;
} AppletSwkbdInline;
void applet_swkbd_inline_init(AppletSwkbdInline *k, const AppletSwkbdConfig *cfg);
/* Handle one request storage; `text` is what the user types when the keyboard appears (NULL = the user cancels). Replies go to call->interactive_out. Returns
 * 0 on a malformed/unknown request (answered with a Default reply, so the game does not hang). */
int applet_swkbd_inline_request(AppletSwkbdInline *k, const uint8_t *req, size_t size, const char *text, LibAppletCall *call);
/* Encodes a reply {u32 type; u32 text bytes; utf8 text...} into a malloc'd interactive output. */
int applet_swkbd_inline_reply(LibAppletCall *call, uint32_t type, const char *text);

/* ---- network connection (NetConnect, 0x0f) -------------------------------------------------------------------------------------------------- */
/* A title launches it when nifm hands back a failed network request: IRequest::GetAppletInfo gives the applet id, the mode and the storages the title
 * pushes (CommonArguments, then {u32 0, Result, Uuid}). On a console it tries to connect, shows the error, and closes when the user leaves.
 * Output (firmware 22.1.0 netConnect +0x245d80 / +0x245da0, written at its exit +0x314cec): one 8-byte storage {u32 0; Result}, where the Result
 * is ErrorHandlingCompleted (2110-0190) when a connection was made and 2110-2900 when the user left without one. The SDK reads it with
 * nn::la::AppletToNifmArgumentsReader (Result at offset 4). Its NetworkConnection::HandleNetworkRequestResult turns a 0 Result into 0190, and
 * a title resubmits on 0190, so 0 means "connected, try again".
 * This host has no network to offer, so the applet closes at once, as it does when the user backs out on a console that cannot connect: it
 * finishes normally (GetResult success) with 2110-2900, the title's HandleNetworkRequestResult fails, and the game shows its own offline error.
 * (Firmware applets also push a 4-byte {s32 exit code} storage first when the caller's la version is 5 or more. The SDK maps it to the same
 * two Results; it is not pushed here.) */
enum { LIBAPPLET_ID_NET_CONNECT = 0x0f };
#define APPLET_NETCONNECT_OUT_SIZE 8u
#define APPLET_NETCONNECT_R_CONNECTED HZ_RESULT(HZ_MOD_NIFM, 190)      /* nn::nifm ErrorHandlingCompleted */
#define APPLET_NETCONNECT_R_NOT_CONNECTED HZ_RESULT(HZ_MOD_NIFM, 2900) /* the user left netConnect without a connection */
void applet_netconnect_encode(uint32_t result, uint8_t out[APPLET_NETCONNECT_OUT_SIZE]); /* {u32 0; Result} */
int applet_netconnect_handler(LibAppletCall *call); /* always answers (1) */
int applet_netconnect_register(void);               /* 0 ok, -1 table full */

/* ---- error applet hook --------------------------------------------------------------------------------------------------------------------- */
/* The frontend can surface the error message; the default is none (the runtime then falls back to the log line and the generic alert). */
typedef void (*AppletErrorHook)(const AppletErrorInfo *e, const char *code_text, const char *description);
void applet_set_error_hook(AppletErrorHook hook);
int applet_error_notify(const AppletErrorInfo *e, const char *code_text, const char *description); /* 1 when a hook handled it */

/* ---- player select: the profile list ------------------------------------------------------------------------------------------------------- */
/* The account service registers its profile list getter (default: none, the caller supplies the one local user). */
typedef unsigned (*AppletProfileLister)(uint8_t (*uids)[16], unsigned max);
void applet_set_profile_lister(AppletProfileLister lister);
unsigned applet_profiles(uint8_t (*uids)[16], unsigned max, const uint8_t fallback[16]);
/* NAPPLE_PLAYER_SELECT: "cancel", "<index>", or "<hex uid prefix>" (not parsed here: index only). Returns 1 for cancel; *index = wanted profile. */
int applet_player_select_spec(const char *spec, int *index);

/* ---- interactive sheets (the frontend registers them; NULL = headless answers) --------------------------------------------------------------- */
/* Player select: show the profiles, return the chosen index or -1 when the user cancels. Not consulted when NAPPLE_PLAYER_SELECT is set (scripted). */
typedef int (*AppletPlayerSelectUi)(const uint8_t (*uids)[16], unsigned count);
void applet_set_player_select_ui(AppletPlayerSelectUi fn);
/* Returns -2 when no UI is registered (keep the headless answer), else the UI's answer. */
int applet_player_select_ask(const uint8_t (*uids)[16], unsigned count);
/* Controller support: show the pad-assignment sheet for what the game asked (a->min..max, a->connected pads) and return 1 to confirm (the UI may adjust
 * a->players) or 0 to cancel. Not consulted when NAPPLE_CONTROLLER_SUPPORT is set. */

/* ---- controller support: cancel ------------------------------------------------------------------------------------------------------------ */
enum { APPLET_CONTROLLER_OK = 0, APPLET_CONTROLLER_CANCELLED = 2 };
/* NAPPLE_CONTROLLER_SUPPORT: "cancel" cancels; "strict" cancels when fewer pads are connected than the game's minimum (the real applet would keep waiting). */
void applet_controller_encode_result(const AppletControllerAnswer *a, uint32_t result, uint8_t out[APPLET_CONTROLLER_OUT_SIZE]);
typedef int (*AppletControllerUi)(AppletControllerAnswer *a);
void applet_set_controller_ui(AppletControllerUi fn);
int applet_controller_ask(AppletControllerAnswer *a); /* -2 when no UI is registered, else 1 confirmed / 0 cancelled */
void applet_controller_apply_policy(const char *spec, AppletControllerAnswer *a, uint32_t *result);
