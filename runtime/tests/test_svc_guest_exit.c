/* main.c's clean guest exit (the SDK 15 nn::init::Start finalizer callback): run in a forked child against fake modules whose finalizers write a
 * marker each into a pipe. Checks the order (reverse load order; DT_FINI when a module has one, else fini_array backwards; an unloaded NRO skipped,
 * empty and -1 entries skipped), that the process exits 0 by itself, and the run summary it leaves. macOS (the runtime objects). */
#define main runtime_main_under_test
#include "../src/main.c"
#undef main
#include "svc_test.h"
#include <sys/wait.h>

void frontend_gfx_ready(void) {}
void frontend_wait_shader_prewarm(const char *dir) { (void)dir; }

static int g_pipe = -1;
static void mark(char c) { (void)!write(g_pipe, &c, 1); }
static void fa(void) { mark('a'); }
static void fb(void) { mark('b'); }
static void fc(void) { mark('c'); }
static void fx(void) { mark('x'); } /* must never run: the module is gone */
static void fs_(void) { mark('S'); }
static void f0(void) { mark('0'); }

int main(void) {
    static uint64_t main_img[8], sub_img[8], gone_img[8], sdk_img[8];
    static Module mods[MAX_MODULES];
    g_modules = mods;
    /* main: fini_array [a, b, 0, c] -> c b a */
    main_img[2] = (uint64_t)(uintptr_t)fa, main_img[3] = (uint64_t)(uintptr_t)fb, main_img[4] = 0, main_img[5] = (uint64_t)(uintptr_t)fc;
    g_modules[0] = (Module){.name = "main", .base = (uint8_t *)main_img, .fini_array = 16, .fini_array_size = 32};
    /* subsdk0: fini_array [0] with a -1 terminator style entry -> 0 */
    sub_img[0] = ~0ull, sub_img[1] = (uint64_t)(uintptr_t)f0;
    g_modules[1] = (Module){.name = "subsdk0", .base = (uint8_t *)sub_img, .fini_array = 0, .fini_array_size = 16};
    /* an NRO that ldr:ro unloaded again */
    gone_img[0] = (uint64_t)(uintptr_t)fx;
    g_modules[2] = (Module){.name = "nro0", .base = (uint8_t *)gone_img, .fini_array = 0, .fini_array_size = 8, .dynamic = 1, .gone = 1};
    /* sdk: DT_FINI wins over its fini_array */
    sdk_img[0] = (uint64_t)(uintptr_t)fx;
    g_modules[3] = (Module){.name = "sdk", .base = (uint8_t *)sdk_img, .dt_fini = (uint64_t)((uintptr_t)fs_ - (uintptr_t)sdk_img), .fini_array = 0, .fini_array_size = 8};
    g_nmodules = 4;

    char dir[] = "/tmp/napple-guest-exit-XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "temp dir");
    char sum[256];
    snprintf(sum, sizeof sum, "%s/runtime-boot.json", dir);
    int p[2];
    CHECK(pipe(p) == 0, "pipe");
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        g_pipe = p[1];
        summary_path = sum;
        summary_set_title("0100000000000000", "0000000000000000");
        cb_finis();
        _exit(77); /* never reached: the exit path must not return */
    }
    close(p[1]);
    char order[32] = {0};
    size_t n = 0;
    ssize_t r;
    while (n < sizeof order - 1 && (r = read(p[0], order + n, sizeof order - 1 - n)) > 0) n += (size_t)r;
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "the guest exit path exits the process with 0 (status %#x)", st);
    CHECK(!strcmp(order, "S0cba"), "finalizers in reverse load order, DT_FINI first, gone NRO skipped, null / -1 entries skipped (ran \"%s\", expected \"S0cba\")",
          order);
    char js[4096] = {0};
    FILE *f = fopen(sum, "r");
    if (f) {
        (void)!fread(js, 1, sizeof js - 1, f);
        fclose(f);
    }
    CHECK(strstr(js, "\"status\":\"partial\"") && strstr(js, "guest exited normally") && strstr(js, "\"last_stage\":\"exit\""),
          "run summary: partial, \"guest exited normally\", last stage exit (%.200s)", js);
    unlink(sum);
    rmdir(dir);
    printf("{\"test\":\"svc_guest_exit\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
