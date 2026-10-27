/* Linux stand-ins for the macOS-only symbols applet.c reaches (window, input backend, dyld); used only by tests/linux/run_applet.sh. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
void gfx_set_hidden(int hidden) { (void)hidden; }
struct napple_input *hid_input_get(void) { return 0; }
void *frontend_metal_layer(void) { return 0; }
int _NSGetExecutablePath(char *buf, uint32_t *size) {
    ssize_t n = readlink("/proc/self/exe", buf, *size - 1);
    if (n < 0) return -1;
    buf[n] = 0;
    return 0;
}
