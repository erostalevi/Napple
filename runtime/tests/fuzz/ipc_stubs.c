/* Linux stand-ins for the host facts, frontend and input hooks the services call (the ipc fuzz target links the services without their macOS back ends;
 * the same set as the top of tests/test_svc_applet_libs.c plus the few that the hid/audio-free service set still reaches). */
#include "../../src/napple.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void host_display_version(char out[16]) { snprintf(out, 16, "1.0.0"); }
uint64_t host_desired_language(void) { return 0x53552d6e65ULL; }
const char *host_user_name(void) { return "Player"; }
void napple_display_game_size(uint32_t w, uint32_t h, uint32_t *gw, uint32_t *gh) { *gw = w, *gh = h; }
int frontend_text_input(const char *a, const char *b, const char *c, const char *d, unsigned e, unsigned f, int g, int h, char *out, unsigned cap) {
    (void)a, (void)b, (void)c, (void)d, (void)e, (void)f, (void)g, (void)h;
    if (cap) out[0] = 0;
    return 0;
}
size_t napple_utf16_to_utf8(const uint16_t *u, size_t n, char *out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < cap; i++) out[k++] = (char)u[i];
    if (cap) out[k] = 0;
    return k;
}
size_t napple_utf8_to_utf16(const char *s, uint16_t *out, size_t cap) {
    size_t k = 0;
    for (; s[k] && k < cap; k++) out[k] = (uint8_t)s[k];
    return k;
}
void frontend_alert(const char *t, const char *m) { (void)t, (void)m; }
int frontend_connected_pads(void) { return 1; }
void hid_apply_system_common_policy(void) {}
void host_symbols_register(const HostSymbol *table, size_t count) { (void)table, (void)count; }
void *host_symbol_lookup(const char *name) {
    (void)name;
    return NULL;
}
