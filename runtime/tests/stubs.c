/* Symbols that main.c / app.m provide in the real binary, for the unit tests that link the runtime objects without them. */
#include <stdlib.h>
#include "napple.h"
#include "frontend.h"
RuntimeConfig g_cfg;
void *frontend_metal_layer(void) { return NULL; }
int frontend_has_focus(void) { return 1; }
void frontend_frame_presented(void) {}
void frontend_quit(int code) { exit(code); }
int frontend_run(FrontendBoot boot, int headless, int seconds) { (void)boot; (void)headless; (void)seconds; return 0; }
int frontend_text_input(const char *header, const char *sub, const char *guide, const char *initial, unsigned max_len, unsigned min_len, int password, int numeric, char *out, unsigned cap) { (void)header; (void)sub; (void)guide; (void)initial; (void)max_len; (void)min_len; (void)password; (void)numeric; if (cap) out[0] = 0; return 0; }
void frontend_alert(const char *title, const char *message) { (void)title; (void)message; }
int frontend_connected_pads(void) { return 1; }
