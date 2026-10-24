/* Facts about the host the services report to the guest (host_info.c). */
#pragma once
#include <stdint.h>

uint64_t host_desired_language(void);          /* Switch language code as 8 ASCII bytes ("en-US", "ja", ...) */
void host_display_version(char out[16]);       /* application display version, from control.nacp when the data directory has one */
void applet_push_message(uint32_t message);    /* applet.c: queue an AppletMessage for the guest */
const char *host_user_name(void);
const char *host_save_dir(void);               /* <save-root>/<titleid>/save, created on demand */
