/* Logging, error trap and guest memory helpers. */
#include "internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Thread_local jmp_buf *napple_audio_trap;

void napple_audio_log(const napple_audio_host *host, const char *fmt, ...) {
    char line[512];
    int n = snprintf(line, sizeof line, "[napple] audio: ");
    va_list ap; va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    if (host && host->log) { host->log(host->user, line); return; }
    fprintf(stderr, "%s\n", line);
}

_Noreturn void napple_audio_fatal(const char *msg) {
    napple_audio_log(NULL, "protocol error: %s", msg);
    if (napple_audio_trap) longjmp(*napple_audio_trap, 1);
    abort();
}

void *napple_audio_guest(const napple_audio_host *h, uint64_t addr, uint64_t size) {
    if (!addr) return NULL;
    if (h && h->translate) return h->translate(h->user, addr, size);
    return (void *)(uintptr_t)addr;
}

void napple_audio_write_prepare(const napple_audio_host *h, void *p, size_t len) {
    if (h && h->write_prepare && p && len) h->write_prepare(h->user, p, len);
}
