/* Internal interface of the overlay's Online tab (overlay_online.m, overlay_online_stub.m). */
#ifndef NAPPLE_OVERLAY_ONLINE_H
#define NAPPLE_OVERLAY_ONLINE_H
#import <Cocoa/Cocoa.h>
#include "napple_online_ui.h"
#include "fe_internal.h"

/* The Online tab's content view (rebuilt by the tab itself on every state change). fe may be nil in tests. */
NSView *fe_online_page(NappleFE *fe);
/* Active table: the installed one, else the stub. */
const napple_online_ui_api *fe_online_api(void);
/* Sub-tabs registered by overlay-social: count and access. */
NSUInteger fe_online_subtab_count(void);
NSString *fe_online_subtab_title(NSUInteger i);
NSView *fe_online_subtab_make(NSUInteger i);
napple_online_page_fn fe_online_subtab_fn(NSUInteger i); /* the page maker itself (tests call it with their own table) */
/* Test / screenshot aids: show a named screen (signin, signup, verify, reset_start, reset_finish, home, profile, tier, profiles, devices, saves,
   delete, delete_confirm) in the current page; "error:<text>" / "busy:<text>" set the status line first. */
void fe_online_debug_screen(NSString *name, NSString *status);
/* Stub control for tests: reset to signed-out (0) or signed-in (1) canned data. */
void fe_online_stub_reset(int signed_in);
/* Text-field answer hook for tests: values for form keys (email, password, ...) set directly. */
void fe_online_debug_set(NSString *key, NSString *value);
int fe_online_missing_icon_symbols(void);
#endif
