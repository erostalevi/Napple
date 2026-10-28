/* Frontend hooks of the web and album library applets (portable header, no implementation of a UI). Each hook has a null default: the applets
 * then answer headlessly (web: the user leaves at once, album: nothing is picked). The Mac frontend registers a WKWebView / image viewer later. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Open a page and block until the user leaves it. `url` is an http(s) URL or a file:// URL of an offline document. Fill `last_url` (cap bytes,
 * the page the user was on when leaving) and return a WebExitReason (applet_web_core.h), or -1 when the page could not be shown (the applet then
 * reports the error dialog exit). */
typedef int (*AppletUiOpenWeb)(const char *url, int offline, char *last_url, size_t cap);
void applet_ui_set_open_web(AppletUiOpenWeb fn);
int applet_ui_open_web(const char *url, int offline, char *last_url, size_t cap); /* -2 when no hook is set */

/* Show an album image (RGBA8, w x h) and let the user pick it or go back. Return 1 when the user selected this image, 0 for the next one, -1 to close the album. */
typedef int (*AppletUiShowImage)(const uint8_t *rgba, unsigned w, unsigned h, unsigned index, unsigned count);
void applet_ui_set_show_image(AppletUiShowImage fn);
int applet_ui_show_image(const uint8_t *rgba, unsigned w, unsigned h, unsigned index, unsigned count); /* 0 when no hook is set */

/* Scripted (headless / e2e) answers, used before the hooks. NAPPLE_WEB_ANSWER = back | closed | requested | error | <WebExitReason number>; the page left on is
 * the one opened. Returns -2 when the spec is unset/unknown, else the exit reason (or -1 for "error"). */
int applet_web_script(const char *spec);
/* NAPPLE_ALBUM_PICK = <index> (select that image, 0-based) | none (leave at once) | next (skip every image: view mode runs through) | first.
 * Returns -3 when unset/unknown (ask the hook), else what the hook would have answered for image `index` of `count`: 1 select, 0 next, -1 leave. */
int applet_album_script(const char *spec, unsigned index, unsigned count);
/* 1 while album_run is in select mode (the viewer shows a Select button), else 0: read it inside the show-image hook. */
int applet_ui_album_selecting(void);
