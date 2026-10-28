/* Shared support for the fuzz targets: the scratch directory, and (for the targets that link the host stand-ins) runtime_fail that unwinds to the harness
 * instead of aborting, because the services answer a malformed guest request with runtime_fail by design: that is a rejection, not a crash. */
#include "fuzz.h"
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* Scratch files go to tmpfs when there is one (/dev/shm: a file create + write + close costs 2 ms on a disk-backed overlay, 40 us on tmpfs), else build/fuzz/work. */
const char *fuzz_workdir(void) {
    static char dir[512];
    if (!dir[0]) {
        const char *e = getenv("FUZZ_WORKDIR");
        const char *base = e && *e ? e : access("/dev/shm", W_OK) == 0 ? "/dev/shm/napple-fuzz" : "build/fuzz/work";
        snprintf(dir, sizeof dir, "%s/%s", base, fuzz_target.name);
        for (char *c = dir + 1; *c; c++)
            if (*c == '/') {
                *c = 0;
                mkdir(dir, 0755);
                *c = '/';
            }
        mkdir(dir, 0755);
    }
    return dir;
}

jmp_buf *fuzz_jmp; /* armed by a harness around a call that may runtime_fail */
char fuzz_fail_msg[256];
#ifdef FUZZ_RUNTIME_FAIL
_Noreturn void runtime_fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fuzz_fail_msg, sizeof fuzz_fail_msg, fmt, ap);
    va_end(ap);
    if (fuzz_jmp) longjmp(*fuzz_jmp, 1);
    fprintf(stderr, "runtime_fail outside a harness call: %s\n", fuzz_fail_msg);
    abort();
}
#endif
