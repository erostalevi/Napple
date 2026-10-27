/* Host for the library-mode runtime: dlopen the applet dylib and run napple_applet_main on a thread of this process (what the iOS game app does for the
 * Mii Maker). usage: inproc_host <libnapple-applet.dylib> <data dir> <save root> [--headless] [--seconds N]
 * The applet's own env (NAPPLE_APPLET_IN/OUT/ID/MODE, NAPPLE_INPUT_SCRIPT, ...) is read from this process's environment. */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int (*g_main)(int, char **);
static int g_argc;
static char **g_argv;
static void *run(void *p) {
    *(int *)p = g_main(g_argc, g_argv);
    return NULL;
}
int main(int argc, char **argv) {
    if (argc < 4) return 2;
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 3; }
    g_main = dlsym(h, "napple_applet_main");
    if (!g_main) { fprintf(stderr, "no napple_applet_main\n"); return 4; }
    char *args[16] = {"napple-applet", argv[2], "--save-root", argv[3]};
    int n = 4;
    for (int i = 4; i < argc && n < 15; i++) args[n++] = argv[i];
    args[n] = NULL;
    g_argc = n;
    g_argv = args;
    int code = -1;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64u << 20);
    pthread_t t;
    pthread_create(&t, &a, run, &code);
    pthread_join(t, NULL);
    fprintf(stderr, "inproc_host: applet returned %d\n", code);
    return 0;
}
