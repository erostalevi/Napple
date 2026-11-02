/* Napple Online client core, C side (docs/guides/napple-online-client.md): implements frontend/include/napple_online_ui.h over the Rust client in
 * crates/napple-cloud (libnapple_cloud.a: shared credentials, auth/profile/social client). Linked like src/cloud/ (CLOUD=1).
 *
 * Threads: actions and the state refresh run on one worker thread; completions and the change callback are queued and delivered by
 * napple_online_ui_pump(), which the host calls on its main thread (the frontend's frame loop). State getters read a cache. */
#pragma once
#include "napple_online_ui.h"

/* Opens the client (home / URLs NULL = environment: NAPPLE_HOME, NAPPLE_ACCOUNT_URL, NAPPLE_SOCIAL_URL, NAPPLE_ONLINE_SERVER) and starts the worker.
 * uid_hex: the local Switch user playing (32 hex digits) or NULL. Idempotent. Returns the table to hand to napple_frontend_set_online_ui, NULL on failure. */
const napple_online_ui_api *napple_online_runtime_start(const char *home, const char *account_url, const char *social_url, const char *uid_hex);
void napple_online_runtime_stop(void);
/* Main thread: delivers queued completions and the change callback. Returns how many were delivered. */
int napple_online_ui_pump(void);
/* Forces a state refresh now (test hook; the worker also refreshes every NAPPLE_ONLINE_POLL_MS, default 30000, while signed in). */
void napple_online_refresh_now(void);
/* The access token for the game's friend service (net_online_social.c) and the acting profile id. 0 = signed out / none. Start the client lazily. */
size_t napple_account_access_token(char *out, size_t n);
size_t napple_account_profile_id(char *out, size_t n);
