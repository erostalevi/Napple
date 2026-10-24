/* Window and event loop. In window mode this is the frontend library (frontend/include/napple_frontend.h): AppKit runs on the
 * process main thread, the guest boots on a thread of its own once the window and its CAMetalLayer exist (on_ready). */
#pragma once
#include <stdint.h>

struct napple_input;
typedef void (*FrontendBoot)(void);
/* Runs `boot` on a large-stack thread (after the window exists, unless headless) and the event loop; returns the process exit code. */
int frontend_run(FrontendBoot boot, int headless, int seconds);
void *frontend_metal_layer(void);                        /* CAMetalLayer* of the game window, NULL when headless */
void frontend_quit(int code);                            /* any thread: end the process after writing the summary */
int frontend_has_focus(void);
void frontend_frame_presented(void);                     /* gfx present hook: FPS title, loading view */
/* main.c boot(), after gfx_init: shows the launch-time shader compile on the loading screen and returns when it is done (window mode). */
void frontend_wait_shader_prewarm(const char *cache_dir);
struct napple_input *hid_input_get(void);                /* services/hid.c: the input backend (created on first use) */
/* Blocking software keyboard (swkbd applet): UTF-8 in/out; returns 1 when accepted. */
int frontend_text_input(const char *header, const char *sub, const char *guide, const char *initial, unsigned max_len, unsigned min_len,
                        int password, int numeric, char *out, unsigned cap);
void audio_init_once(void);                              /* services/audio.c */
