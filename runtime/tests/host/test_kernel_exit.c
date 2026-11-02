/* Host unit test (core-ordering-kernel, 2026-11-02): kernel.c's process-info and process-exit rows.
 *   G19  svcGetInfo 19 InitialProcessIdRange: InvalidEnumValue on this 22.x kernel ([4.0.0-4.1.0] only), whatever the handle / subid
 *   G27  svcGetInfo 27 IoRegionHint: InvalidCombination for a subid other than 0, else InvalidHandle (an application has no io region)
 *   X1   svcExitProcess after nnMain returned (the 2-callback nn::init::Start ends the process itself): summary "partial", exit 0
 *   X2   svcExitProcess from inside nnMain, or with no nn::init::Start frame: summary "fail" as before, exit 0
 * kernel.c is included with summary_write renamed so the forked child can hand the summary it writes to the parent. The guest modules are fakes
 * (one module whose symbol table holds nnMain and nn::init::Start); guest memory is host memory here, so the frame records live in a host array. */
#define summary_write test_summary_write
#include "../../src/kernel.c"
#undef summary_write
#include "host_stubs.h"
#include "../check.h"
#include <sys/wait.h>

void svc_threads_register(void);

/* ---- fake modules (in place of host_nomodules.c) ---- */
Module *g_modules;
unsigned g_nmodules;
static uint8_t text[0x1000] __attribute__((aligned(16)));
static const char strtab[] = "\0nnMain\0_ZN2nn4init5StartEmmPFvvES2_\0helper";
static const Sym syms[] = {
    {.name = 1, .binding = 1, .type = 2, .defined = 1, .value = 0x100, .size = 0x100},  /* nnMain        0x100-0x200 */
    {.name = 8, .binding = 1, .type = 2, .defined = 1, .value = 0x400, .size = 0x100},  /* nn::init::Start 0x400-0x500 */
    {.name = 38, .binding = 1, .type = 2, .defined = 1, .value = 0x600, .size = 0x40},  /* helper (e.g. nn::os quick exit) */
};
static Module mods[1];
const Module *module_by_name(const char *name) { return g_nmodules && !strcmp(name, mods[0].name) ? &mods[0] : NULL; }
const Module *module_for_address(uint64_t a) {
    uint64_t b = (uintptr_t)mods[0].base;
    return g_nmodules && a >= b && a < b + mods[0].size ? &mods[0] : NULL;
}
void module_describe(uint64_t a, char *out, size_t n) { snprintf(out, n, "0x%llx", (unsigned long long)a); }
int module_pc_is_guest_code(uint64_t pc) { return module_for_address(pc) != NULL; }

/* ---- the summary the child writes ---- */
static int g_pipe = -1;
void test_summary_write(const char *status, const char *reason) {
    char b[300];
    int n = snprintf(b, sizeof b, "%s|%s", status, reason);
    if (g_pipe >= 0) (void)!write(g_pipe, b, (size_t)n);
}

static uint64_t T(uint64_t off) { return (uintptr_t)text + off; }
static uint64_t frames[16][2] __attribute__((aligned(16))); /* {next fp, return address} */
static uint64_t fp_of(int i) { return (uintptr_t)frames[i]; }

/* Runs svcExitProcess with this lr / fp in a forked child; returns the "status|reason" it wrote, and the exit status. */
static int exit_in_child(uint64_t lr, uint64_t fp, char *out, size_t cap) {
    int p[2];
    if (pipe(p)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        g_pipe = p[1];
        uint64_t r[31] = {0};
        r[29] = fp;
        r[30] = lr;
        svc_dispatch(0x07, r, "test", 0x40);
        _exit(77); /* never reached: ExitProcess does not return */
    }
    close(p[1]);
    size_t n = 0;
    ssize_t k;
    while (n < cap - 1 && (k = read(p[0], out + n, cap - 1 - n)) > 0) n += (size_t)k;
    out[n] = 0;
    close(p[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int main(void) {
    svc_kernel_register();
    svc_threads_register();
    static uint8_t tlr_main[0x200];
    host_thread_attach(tlr_main);

    /* G19 */
    for (uint64_t sub = 0; sub < 3; sub++) {
        uint64_t r[9] = {0, 19, 0, sub};
        svc_dispatch(0x29, r, "test", 0x40);
        CHECK(r[0] == R_INVALID_ENUM, "GetInfo 19 InitialProcessIdRange subid %llu, handle 0: InvalidEnumValue on a 5.0.0+ kernel (%#llx)", (unsigned long long)sub,
              (unsigned long long)r[0]);
    }
    { uint64_t r[9] = {0, 19, 0xffff8001, 0}; svc_dispatch(0x29, r, "test", 0x40); CHECK(r[0] == R_INVALID_ENUM, "GetInfo 19 with the process pseudo-handle: the firmware check comes first (%#llx)", (unsigned long long)r[0]); }

    /* G27 */
    {
        uint64_t r[9] = {0, 27, 0xffff8001, 0};
        svc_dispatch(0x29, r, "test", 0x40);
        CHECK(r[0] == R_INVALID_HANDLE, "GetInfo 27 IoRegionHint subid 0, process handle: not an io region -> InvalidHandle (%#llx)", (unsigned long long)r[0]);
        uint64_t e[9] = {0};
        svc_dispatch(0x45, e, "test", 0x40); /* CreateEvent: a real handle of the wrong kind */
        uint64_t r2[9] = {0, 27, e[1], 0};
        svc_dispatch(0x29, r2, "test", 0x40);
        CHECK(e[0] == 0 && r2[0] == R_INVALID_HANDLE, "GetInfo 27 with an event handle: InvalidHandle (%#llx)", (unsigned long long)r2[0]);
        uint64_t r3[9] = {0, 27, 0xffff8001, 1};
        svc_dispatch(0x29, r3, "test", 0x40);
        CHECK(r3[0] == R_INVALID_COMBINATION, "GetInfo 27 subid 1: InvalidCombination, checked before the handle (%#llx)", (unsigned long long)r3[0]);
    }
    /* neighbours unchanged */
    { uint64_t r[9] = {0, 18, 0xffff8001, 0}; svc_dispatch(0x29, r, "test", 0x40); CHECK(r[0] == 0, "GetInfo 18 ProgramId still answers"); }
    { uint64_t r[9] = {0, 99, 0, 0}; svc_dispatch(0x29, r, "test", 0x40); CHECK(r[0] == R_INVALID_ENUM, "an unknown GetInfo id stays InvalidEnumValue"); }

    /* exit classification */
    CHECK(!kernel_exit_after_nnmain(0, T(0x480)), "no modules loaded: not a clean return");
    mods[0] = (Module){.name = "main", .base = text, .size = sizeof text, .text_end = sizeof text, .nsym = 3, .syms = syms, .strtab = strtab};
    g_modules = mods;
    g_nmodules = 1;
    /* nnMain returned into Start, which called a helper that issues the svc: lr in the helper, its frame returns into Start, then the host */
    frames[0][0] = fp_of(1), frames[0][1] = T(0x488);
    frames[1][0] = 0, frames[1][1] = 0x7000; /* host return address: the walk ends */
    CHECK(kernel_exit_after_nnmain(fp_of(0), T(0x610)), "helper <- Start: nnMain has returned");
    CHECK(kernel_exit_after_nnmain(0, T(0x4a0)), "the svc issued from Start itself (lr in Start, no frame records)");
    /* quitting from inside nnMain: Start <- nnMain <- helper */
    frames[2][0] = fp_of(3), frames[2][1] = T(0x150);
    frames[3][0] = fp_of(1), frames[3][1] = T(0x488);
    CHECK(!kernel_exit_after_nnmain(fp_of(2), T(0x610)), "nnMain still on the call chain: not a return");
    CHECK(!kernel_exit_after_nnmain(0, T(0x180)), "lr inside nnMain");
    /* no Start frame: a worker thread, or before nnMain */
    frames[4][0] = 0, frames[4][1] = T(0x620);
    CHECK(!kernel_exit_after_nnmain(fp_of(4), T(0x610)), "no nn::init::Start on the chain: not a return");
    /* a broken chain (a frame record pointing down the stack) stops the walk */
    frames[6][0] = fp_of(5), frames[6][1] = T(0x620);
    frames[5][0] = 0, frames[5][1] = T(0x488);
    CHECK(!kernel_exit_after_nnmain(fp_of(6), T(0x610)), "a frame record that goes down the stack ends the walk");

    /* X1 / X2: the summaries ExitProcess writes */
    char got[400];
    int rc = exit_in_child(T(0x610), fp_of(0), got, sizeof got);
    CHECK(rc == 0 && !strncmp(got, "partial|", 8) && strstr(got, "guest exited normally"), "X1 ExitProcess after nnMain returned: exit 0, summary partial (rc %d, \"%s\")", rc, got);
    rc = exit_in_child(T(0x610), fp_of(2), got, sizeof got);
    CHECK(rc == 0 && !strncmp(got, "fail|", 5), "X2 ExitProcess from inside nnMain: summary fail (rc %d, \"%s\")", rc, got);
    rc = exit_in_child(T(0x610), fp_of(4), got, sizeof got);
    CHECK(rc == 0 && !strncmp(got, "fail|", 5), "X2 ExitProcess with no nn::init::Start frame: summary fail (rc %d, \"%s\")", rc, got);

    printf("{\"test\":\"kernel_exit\",\"checked\":%d,\"failed\":%d}\n", g_checked, g_failed);
    return g_failed != 0;
}
