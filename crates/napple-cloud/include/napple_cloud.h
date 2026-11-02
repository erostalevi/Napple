/* napple-cloud C API (crates/napple-cloud/src/ffi.rs): Napple Account cloud saves for the native runtime (macOS and iOS).
 * Link target/<triple>/release/libnapple_cloud.a and -framework Security. Every function is safe to call when signed out (no-ops). */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*napple_cloud_log_fn)(const char *line);
typedef void (*napple_cloud_event_fn)(const char *event_json, void *ctx);

/* Call before the game opens any save. support_dir: <support>/cloud/<namespace>/ holds the account state; save_dir: <save root>/<TITLE>/save;
 * uids: comma-separated local user ids (32 hex digits). Pulls each linked user's newest cloud save (giving up after pull_timeout_ms, then the
 * game starts from the local save) and starts the background push thread. Returns 1 when cloud saves are active for this title, else 0
 * (signed out, title turned off, no linked user: nothing else happens). */
int napple_cloud_runtime_start(const char *support_dir, const char *save_dir, const char *title_id, const char *uids, int pull_timeout_ms,
                               napple_cloud_log_fn log);
/* A save was committed: pushed in the background after NAPPLE_CLOUD_DEBOUNCE_MS (default 5000) without further commits. Never blocks. */
void napple_cloud_runtime_commit(void);
/* Clean exit: push now, wait at most timeout_ms. 1 = done, 0 = still running (it stays queued for the next launch) or not active. */
int napple_cloud_runtime_flush(int timeout_ms);
/* {"active":bool,"title_id":..,"targets":[{"profile_id","status","version","detail","upgrade_url"}]}; valid until the next call. */
const char *napple_cloud_runtime_status(void);
/* Events as JSON {"event": "pulled"|"pushed"|"conflict"|"plan_limit"|"restored", "message", "title_id", "profile_id", ...}; called from
 * the cloud thread (or the launch thread during the pull). */
void napple_cloud_set_event_callback(napple_cloud_event_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif
