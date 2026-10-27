/* Library mode (NAPPLE_LIB): how a guest ends inside a host process. See napple.h GUEST_EXIT. */
#ifdef NAPPLE_LIB
#include "napple.h"
#include "napple_input.h"
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

/* What the host hands this image before napple_applet_main (windowed mode): its CAMetalLayer, which the applet draws into instead of a window of its
 * own, and its napple_input, whose merged pad state is mirrored into this image's own HID shared memory (the applet guest reads that one). */
static void *g_host_layer;
static napple_input *g_host_input;
__attribute__((visibility("default"))) void napple_applet_attach(void *metal_layer, void *host_input) {
    g_host_layer = metal_layer;
    g_host_input = host_input;
}
void *lib_host_layer(void) { return g_host_layer; }
static void *mirror_thread(void *arg) {
    napple_input *mine = arg;
    napple_input_focus(mine, 1);
    while (g_host_input) {
        napple_input_snapshot s;
        napple_input_get_snapshot(g_host_input, &s);
        napple_input_virtual_pad(mine, s.pad.buttons, s.pad.left.x / 32767.f, s.pad.left.y / 32767.f, s.pad.right.x / 32767.f, s.pad.right.y / 32767.f);
        usleep(4000);
    }
    return NULL;
}
void lib_mirror_host_input(void *mine) {
    if (!g_host_input) return;
    pthread_t t;
    pthread_create(&t, NULL, mirror_thread, mine);
    pthread_detach(t);
}

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_ended, g_code;

_Noreturn void lib_guest_end(int code) {
    pthread_mutex_lock(&g_mu);
    if (!g_ended) {
        g_ended = 1;
        g_code = code;
    }
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    pthread_exit(NULL); /* the other guest threads stay parked; the host drops this image's work */
}

int lib_wait_guest_end(int seconds) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += seconds > 0 ? seconds : 1 << 28;
    pthread_mutex_lock(&g_mu);
    while (!g_ended)
        if (pthread_cond_timedwait(&g_cv, &g_mu, &until)) break;
    int code = g_ended ? g_code : 0;
    pthread_mutex_unlock(&g_mu);
    return code;
}
#endif
