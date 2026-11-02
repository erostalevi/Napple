/* Runtime glue for cloud saves: see cloud.h and crates/napple-cloud/src/ffi.rs.
 * Environment: NAPPLE_CLOUD=0 turns it off for a run; NAPPLE_CLOUD_PULL_TIMEOUT_MS (default 8000) bounds the pull before mount;
 * NAPPLE_CLOUD_EXIT_TIMEOUT_MS (default 10000) bounds the push at exit; NAPPLE_CLOUD_DEBOUNCE_MS (default 5000) is the quiet time after a
 * commit before the background push; NAPPLE_ACCOUNT_NAMESPACE / NAPPLE_ACCOUNT_KEYCHAIN select another sign-in (tests: two devices on one Mac). */
#include "cloud.h"
#include "napple.h"
#include "account_core.h"
#include "napple_cloud.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const uint8_t g_user_id[16];
static int g_cloud_on;

static void cloud_log(const char *line) { LOG("cloud", "%s", line); }
/* host_save_dir() without creating it (signed out, nothing on disk changes) */
static const char *save_dir(void) {
    static char d[1100];
    snprintf(d, sizeof d, "%s/%s/save", g_cfg.save_root, g_cfg.title_id);
    return d;
}
static int env_int(const char *name, int def) {
    const char *e = getenv(name);
    return e && *e ? atoi(e) : def;
}

void cloud_runtime_start(void) {
#ifdef NAPPLE_LIB
    return; /* an in-process firmware applet: the host game owns cloud sync */
#endif
    const char *off = getenv("NAPPLE_CLOUD");
    if (off && !strcmp(off, "0")) return;
    if (getenv("NAPPLE_APPLET_PARENT")) return; /* a firmware applet child process (Mii Maker, ...): not a game save */
    if (!g_cfg.title_id[0]) return;
    /* the local users acc:u0 offers (account.c): the first one is g_user_id, NAPPLE_PROFILES adds more */
    char uids[ACCOUNT_MAX_PROFILES * 33 + 1];
    size_t n = 0;
    unsigned count = account_profile_count_spec(getenv("NAPPLE_PROFILES"));
    for (unsigned i = 0; i < count; i++) {
        uint8_t u[16];
        account_profile_uid(g_user_id, i, u);
        for (int k = 0; k < 16; k++) n += (size_t)snprintf(uids + n, sizeof uids - n, "%02x", u[k]);
        if (i + 1 < count) uids[n++] = ',';
    }
    uids[n] = 0;
    g_cloud_on = napple_cloud_runtime_start(napple_support_dir(), save_dir(), g_cfg.title_id, uids, env_int("NAPPLE_CLOUD_PULL_TIMEOUT_MS", 8000),
                                            cloud_log);
}

void cloud_on_commit(const char *root) {
    if (!g_cloud_on || !root) return;
    char pre[1200];
    snprintf(pre, sizeof pre, "%s/space1/", save_dir()); /* user saves (account + device); not system/bcat/cache/temporary roots */
    if (strncmp(root, pre, strlen(pre))) return;
    napple_cloud_runtime_commit();
}

void cloud_on_exit(void) {
    if (!g_cloud_on) return;
    g_cloud_on = 0; /* once: frontend_quit and guest_exit_clean can both get here */
    napple_cloud_runtime_flush(env_int("NAPPLE_CLOUD_EXIT_TIMEOUT_MS", 10000));
}
