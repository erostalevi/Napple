/* Napple Online UI API: the function table the runtime (client-core) provides to the frontend overlay's Online tab.
   No link-time coupling: the runtime fills a napple_online_ui_api and hands it over with napple_frontend_set_online_ui();
   with no table installed the overlay uses its built-in stub (canned data, no servers).
   Contract: `docs/guides/napple-social.md`. All functions are called on the main thread. State getters copy into caller storage.
   Async actions return a request id (> 0) at once and later call the completion on the main thread. */
#ifndef NAPPLE_ONLINE_UI_H
#define NAPPLE_ONLINE_UI_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NAPPLE_ONLINE_UI_VERSION 1
#define NAPPLE_ONLINE_MAX_PROFILES 8
#define NAPPLE_ONLINE_MAX_LINKS 3
#define NAPPLE_ONLINE_ICON_COUNT 48

/* Visibility tiers (per profile). */
typedef enum napple_online_tier {
    NAPPLE_TIER_PUBLIC = 1,         /* found by everyone, friend requests from anyone */
    NAPPLE_TIER_FRIENDS_SEARCH = 2, /* Friends (searchable) */
    NAPPLE_TIER_FRIENDS_HIDDEN = 3, /* Friends (hidden) */
    NAPPLE_TIER_PRIVATE = 4,
    NAPPLE_TIER_GHOST = 5
} napple_online_tier;

/* Result codes of the completion callback. */
typedef enum napple_online_result {
    NAPPLE_ONLINE_OK = 0,
    NAPPLE_ONLINE_ERR_NETWORK = 1,     /* server unreachable / timeout */
    NAPPLE_ONLINE_ERR_REJECTED = 2,    /* 4xx: json has {"reason":"..."} */
    NAPPLE_ONLINE_ERR_UNVERIFIED = 3,  /* login of an unverified account: a fresh code was mailed */
    NAPPLE_ONLINE_ERR_RATE_LIMITED = 4,
    NAPPLE_ONLINE_ERR_INVALID = 5,     /* local validation failed */
    NAPPLE_ONLINE_ERR_INTERNAL = 6
} napple_online_result;

typedef struct napple_online_link {
    char kind[12];   /* discord | instagram | x | web */
    char value[128]; /* web must be https */
} napple_online_link;

typedef struct napple_online_profile {
    char id[40];
    char nickname[24];
    char bio[164];
    napple_online_link links[NAPPLE_ONLINE_MAX_LINKS];
    int link_count;
    int icon_id;          /* built-in 0..47 */
    char photo_path[512]; /* local cached file (256x256 JPEG), "" = none */
    int tier;             /* napple_online_tier */
    char friend_code[20]; /* NP-123456789012 */
} napple_online_profile;

typedef struct napple_online_device {
    char id[40];
    char name[48];
    char platform[16];
    int current;  /* this device */
    int pending;  /* waiting for approval by an existing device */
    int64_t last_seen_unix;
} napple_online_device;

typedef struct napple_online_friend {
    char profile_id[40];
    char nickname[24];
    char photo_path[512];
    int icon_id;
    int online;
    char title_id[20]; /* presence: "" when offline or not shared */
    char mode[40];
    char joinable_token[64]; /* non-empty when the friend can be joined */
} napple_online_friend;

typedef struct napple_online_request_entry {
    char profile_id[40];
    char nickname[24];
    char photo_path[512];
    int icon_id;
    int incoming; /* 1 incoming, 0 outgoing */
} napple_online_request_entry;

typedef struct napple_online_stat {
    char title_id[20];
    char title_name[64];
    char key[40];
    int64_t value;
} napple_online_stat;

typedef struct napple_online_save_status {
    char title_id[20];
    char title_name[64];
    int state;              /* 0 off, 1 in sync, 2 uploading, 3 downloading, 4 conflict, 5 error */
    int64_t last_sync_unix; /* 0 = never */
    int64_t bytes;
} napple_online_save_status;

typedef struct napple_online_account {
    int signed_in;
    char email[96];
    char account_id[40];
    int email_verified;
    napple_online_profile current; /* valid when signed_in */
    int profile_count;
    int server_reachable;
} napple_online_account;

/* Completion: request_id from the action, result_code napple_online_result, json = response body or {"reason":...} (valid during the call only). */
typedef void (*napple_online_done_fn)(void *user, uint64_t request_id, int result_code, const char *json);
/* State changed (sign in/out, profile edit, friends, presence, saves ...): re-read what you show. */
typedef void (*napple_online_change_fn)(void *user);

typedef struct napple_online_ui_api {
    uint32_t version; /* NAPPLE_ONLINE_UI_VERSION */
    void *ctx;        /* first argument of every function */

    /* ---- state queries (copy up to `cap` entries, return the total count available) ---- */
    void (*get_account)(void *ctx, napple_online_account *out);
    size_t (*get_profiles)(void *ctx, napple_online_profile *out, size_t cap);
    size_t (*get_devices)(void *ctx, napple_online_device *out, size_t cap); /* includes pending approvals (pending=1) */
    size_t (*get_friends)(void *ctx, napple_online_friend *out, size_t cap);
    size_t (*get_requests)(void *ctx, napple_online_request_entry *out, size_t cap);
    size_t (*get_blocks)(void *ctx, napple_online_request_entry *out, size_t cap);   /* incoming=0 */
    size_t (*get_recent)(void *ctx, napple_online_request_entry *out, size_t cap);   /* recent players */
    size_t (*get_stats)(void *ctx, napple_online_stat *out, size_t cap);             /* current profile, all titles */
    size_t (*get_save_status)(void *ctx, napple_online_save_status *out, size_t cap);

    /* ---- async actions: each returns a request id (> 0) and later calls done(user, id, result, json) ---- */
    uint64_t (*signup)(void *ctx, const char *email, const char *password, const char *nickname, int age_ok, int terms_ok, napple_online_done_fn done, void *user);
    uint64_t (*verify)(void *ctx, const char *email, const char *code, napple_online_done_fn done, void *user);
    uint64_t (*login)(void *ctx, const char *email, const char *password, napple_online_done_fn done, void *user);
    uint64_t (*logout)(void *ctx, napple_online_done_fn done, void *user);
    uint64_t (*reset_start)(void *ctx, const char *email, napple_online_done_fn done, void *user);
    uint64_t (*reset_finish)(void *ctx, const char *email, const char *code, const char *new_password, napple_online_done_fn done, void *user);
    uint64_t (*switch_profile)(void *ctx, const char *profile_id, napple_online_done_fn done, void *user);
    uint64_t (*create_profile)(void *ctx, const char *nickname, napple_online_done_fn done, void *user);
    /* profile_json: {"nickname","bio","links":[{"kind","value"}],"icon_id"}; fields left out stay as they are. */
    uint64_t (*update_profile)(void *ctx, const char *profile_id, const char *profile_json, napple_online_done_fn done, void *user);
    uint64_t (*upload_photo)(void *ctx, const char *profile_id, const char *path, napple_online_done_fn done, void *user);
    uint64_t (*set_tier)(void *ctx, const char *profile_id, int tier, napple_online_done_fn done, void *user);
    uint64_t (*friend_request)(void *ctx, const char *code_or_profile_id, napple_online_done_fn done, void *user);
    uint64_t (*friend_accept)(void *ctx, const char *profile_id, napple_online_done_fn done, void *user);
    uint64_t (*friend_decline)(void *ctx, const char *profile_id, napple_online_done_fn done, void *user);
    uint64_t (*friend_remove)(void *ctx, const char *profile_id, napple_online_done_fn done, void *user);
    uint64_t (*block)(void *ctx, const char *profile_id, napple_online_done_fn done, void *user);
    uint64_t (*unblock)(void *ctx, const char *profile_id, napple_online_done_fn done, void *user);
    /* json of the completion: {"results":[{"profile_id","nickname","icon_id","photo_path"}...]} */
    uint64_t (*search)(void *ctx, const char *query, napple_online_done_fn done, void *user);
    uint64_t (*invite)(void *ctx, const char *profile_id, napple_online_done_fn done, void *user);
    uint64_t (*join)(void *ctx, const char *profile_id, const char *joinable_token, napple_online_done_fn done, void *user);
    uint64_t (*report)(void *ctx, const char *profile_id, const char *reason, napple_online_done_fn done, void *user);
    uint64_t (*approve_device)(void *ctx, const char *device_id, napple_online_done_fn done, void *user);
    uint64_t (*revoke_device)(void *ctx, const char *device_id, napple_online_done_fn done, void *user);
    uint64_t (*delete_account)(void *ctx, const char *password, napple_online_done_fn done, void *user);

    /* One callback at a time; NULL clears. Called on the main thread. */
    void (*set_change_callback)(void *ctx, napple_online_change_fn fn, void *user);
} napple_online_ui_api;

/* Frontend side (frontend/src/overlay_online.m). api = NULL (or never called) selects the built-in stub with canned data.
   The table is copied; ctx must outlive the frontend. Main thread. */
struct napple_frontend;
void napple_frontend_set_online_ui(struct napple_frontend *fe, const napple_online_ui_api *api);
/* The stub table (canned data, no servers); also usable by tests. */
const napple_online_ui_api *napple_online_ui_stub(void);

/* Sub-tab registration for overlay-social (frontend/src/overlay_online_social*.m): a page maker returns a +1 retained NSView* (as void*, __bridge_retained) for one sub-tab.
   Registered pages appear after Account in the Online tab's sub-tab strip, in registration order, shown only while signed in. */
typedef void *(*napple_online_page_fn)(const napple_online_ui_api *api);
void napple_online_register_subtab(const char *title, napple_online_page_fn make);

#ifdef __cplusplus
}
#endif
#endif
