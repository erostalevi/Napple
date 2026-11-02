/* Social sub-tabs of the overlay Online tab (friends, requests, add/search, blocked, recent, stats) and the toasts for arriving requests / invites.
   Implemented in frontend/src/overlay_online_social.m against napple_online_ui.h only. The tab shell (overlay_online.m) calls these hooks:
   - napple_online_social_init() once at startup: registers the sub-tabs with napple_online_register_subtab() and stores the host callbacks;
   - napple_online_social_changed(api) from its change callback (refreshes the live pages and raises toasts for new incoming requests);
   - napple_online_social_invite_event() when client-core reports an invite (received or accepted). */
#ifndef NAPPLE_ONLINE_SOCIAL_H
#define NAPPLE_ONLINE_SOCIAL_H
#include <stddef.h>
#include "napple_online_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct napple_online_social_host {
    void *user;
    /* Transient message over the game (NappleFE -toast:). NULL = log only. */
    void (*toast)(void *user, const char *text);
    /* On-screen keyboard (swkbd bar): blocking, returns 1 and fills out when the user confirmed. NULL = only the hardware keyboard. */
    int (*text_input)(void *user, const char *title, const char *initial, char *out, size_t cap);
} napple_online_social_host;

void napple_online_social_init(const napple_online_social_host *host);
void napple_online_social_changed(const napple_online_ui_api *api);
/* kind: 0 invite received, 1 invite accepted */
void napple_online_social_invite_event(int kind, const char *nickname, const char *title_name);

#ifdef __cplusplus
}
#endif
#endif
