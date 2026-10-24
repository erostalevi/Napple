/* Logging, the JSON-lines trace and the compatibility summary (see docs/compat-format.md). */
#include "napple.h"
#include "timescale.h"
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int log_verbose;
const char *summary_path;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *trace_file;
static uint64_t t0_ns;

uint64_t runtime_now_ns(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (uint64_t)((__uint128_t)mach_absolute_time() * tb.numer / tb.denom);
}
/* The Horizon system tick runs at 19.2 MHz. It follows the guest clock (timescale.c): game speed changes scale every guest-visible clock at once. */
uint64_t runtime_guest_ticks(void) { return (uint64_t)((__uint128_t)timescale_guest_ns() * 12 / 625); }

void log_init(void) {
    t0_ns = runtime_now_ns();
    filelog_start();
    const char *v = getenv("NAPPLE_VERBOSE");
    log_verbose = v && *v && strcmp(v, "0");
    const char *t = getenv("NAPPLE_TRACE");
    if (t && *t) {
        trace_file = fopen(t, "w");
        if (!trace_file) fprintf(stderr, "[napple] cannot open trace file %s: %s\n", t, strerror(errno));
        else setvbuf(trace_file, NULL, _IOLBF, 0);
    }
}
int trace_enabled(void) { return trace_file != NULL; }

static double since_start(void) { return (double)(runtime_now_ns() - t0_ns) / 1e9; }

void log_line(const char *tag, const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    pthread_mutex_lock(&log_lock);
    fprintf(stderr, "[napple %8.3f] %s: %s\n", since_start(), tag, buf);
    pthread_mutex_unlock(&log_lock);
}

void trace_event(const char *kind, const char *fmt, ...) {
    if (!trace_file) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    pthread_mutex_lock(&log_lock);
    fprintf(trace_file, "{\"t\":%.6f,\"k\":\"%s\"%s%s}\n", since_start(), kind, *buf ? "," : "", buf);
    pthread_mutex_unlock(&log_lock);
}

/* ---- summary ------------------------------------------------------------------------------------------------ */
typedef struct { char service[16]; unsigned cmd; int status; unsigned count, max_bytes; } IpcSeen;
#define MAX_IPC_SEEN 1024
static IpcSeen ipc_seen[MAX_IPC_SEEN];
static unsigned nipc_seen;
static struct { unsigned count; const char *name; } svc_miss[256];
static char nvn_missing[256][96];
static unsigned nnvn_missing;
static char notes[64][200];
static unsigned nnotes;
static char crash_note[256];
static int crash_signal;
static uint64_t crash_pc;
static char g_title[17] = "unknown", g_build[17] = "unknown";
static char stage_name[48] = "start";
static int summary_done;

void summary_set_title(const char *title_id, const char *build_id) {
    snprintf(g_title, sizeof g_title, "%s", title_id);
    snprintf(g_build, sizeof g_build, "%s", build_id);
    filelog_set_title(title_id);
}
void summary_stage(const char *stage) { snprintf(stage_name, sizeof stage_name, "%s", stage); trace_event("stage", "\"stage\":\"%s\"", stage); }

void summary_svc_unsupported(unsigned num, const char *name) {
    if (num > 255) return;
    pthread_mutex_lock(&log_lock);
    if (!svc_miss[num].count++) fprintf(stderr, "[napple %8.3f] svc: unimplemented svc 0x%02x (%s)\n", since_start(), num, name ? name : "?");
    svc_miss[num].name = name;
    pthread_mutex_unlock(&log_lock);
}
void summary_ipc(const char *service, unsigned cmd, int status, unsigned request_bytes) {
    pthread_mutex_lock(&log_lock);
    IpcSeen *e = NULL;
    for (unsigned i = 0; i < nipc_seen; i++)
        if (ipc_seen[i].cmd == cmd && !strncmp(ipc_seen[i].service, service, 15)) { e = &ipc_seen[i]; break; }
    if (!e && nipc_seen < MAX_IPC_SEEN) {
        e = &ipc_seen[nipc_seen++];
        snprintf(e->service, sizeof e->service, "%s", service); e->cmd = cmd; e->status = status;
    }
    if (e) { e->count++; if (request_bytes > e->max_bytes) e->max_bytes = request_bytes; if (status > e->status) e->status = status; }
    pthread_mutex_unlock(&log_lock);
}
void summary_nvn_missing(const char *name) {
    pthread_mutex_lock(&log_lock);
    for (unsigned i = 0; i < nnvn_missing; i++) if (!strcmp(nvn_missing[i], name)) { pthread_mutex_unlock(&log_lock); return; }
    if (nnvn_missing < 256) snprintf(nvn_missing[nnvn_missing++], sizeof nvn_missing[0], "%s", name);
    pthread_mutex_unlock(&log_lock);
}
void summary_note(const char *fmt, ...) {
    pthread_mutex_lock(&log_lock);
    if (nnotes < 64) { va_list ap; va_start(ap, fmt); vsnprintf(notes[nnotes++], sizeof notes[0], fmt, ap); va_end(ap); }
    pthread_mutex_unlock(&log_lock);
}
void summary_crash(int sig, uint64_t pc, const char *note) {
    crash_signal = sig; crash_pc = pc;
    snprintf(crash_note, sizeof crash_note, "%s", note ? note : "");
}

static void jstr(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') { fputc('\\', f); fputc(*s, f); }
        else if ((unsigned char)*s < 0x20) fputc(' ', f);
        else fputc(*s, f);
    }
    fputc('"', f);
}
static void mkdirs(const char *path) {
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
}

/* Writes the runtime-boot stage file of docs/compat-format.md (atomically) when a summary path is configured. */
void summary_write(const char *status, const char *reason) {
    if (!summary_path || summary_done) return;
    summary_done = 1;
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", summary_path);
    mkdirs(summary_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "{\"stage\":\"runtime-boot\",\"status\":"); jstr(f, status);
    fprintf(f, ",\"reason\":"); jstr(f, reason);
    fprintf(f, ",\"evidence\":"); jstr(f, getenv("NAPPLE_EVIDENCE") ? getenv("NAPPLE_EVIDENCE") : "");
    fprintf(f, ",\"time\":%lld,\"tool\":\"runtime\",\"title_id\":", (long long)time(NULL)); jstr(f, g_title);
    fprintf(f, ",\"build_id\":"); jstr(f, g_build);
    fprintf(f, ",\"last_stage\":"); jstr(f, stage_name);
    fprintf(f, ",\"counters\":{\"unsupported_svcs\":[");
    int first = 1;
    for (unsigned i = 0; i < 256; i++) if (svc_miss[i].count) { fprintf(f, "%s%u", first ? "" : ",", i); first = 0; }
    fprintf(f, "],\"missing_ipc\":[");
    first = 1;
    for (unsigned i = 0; i < nipc_seen; i++) if (ipc_seen[i].status == 2) {
        fprintf(f, "%s", first ? "" : ","); first = 0;
        char b[64]; snprintf(b, sizeof b, "%s:%u", ipc_seen[i].service, ipc_seen[i].cmd); jstr(f, b);
    }
    fprintf(f, "],\"stubbed_ipc\":[");
    first = 1;
    for (unsigned i = 0; i < nipc_seen; i++) if (ipc_seen[i].status == 1) {
        fprintf(f, "%s", first ? "" : ","); first = 0;
        char b[64]; snprintf(b, sizeof b, "%s:%u", ipc_seen[i].service, ipc_seen[i].cmd); jstr(f, b);
    }
    fprintf(f, "],\"implemented_ipc\":[");
    first = 1;
    for (unsigned i = 0; i < nipc_seen; i++) if (ipc_seen[i].status == 0) {
        fprintf(f, "%s", first ? "" : ","); first = 0;
        char b[64]; snprintf(b, sizeof b, "%s:%u x%u", ipc_seen[i].service, ipc_seen[i].cmd, ipc_seen[i].count); jstr(f, b);
    }
    fprintf(f, "],\"missing_nvn\":[");
    for (unsigned i = 0; i < nnvn_missing; i++) { fprintf(f, "%s", i ? "," : ""); jstr(f, nvn_missing[i]); }
    fprintf(f, "],\"crashes\":[");
    if (crash_signal) { fprintf(f, "{\"signal\":%d,\"pc\":\"0x%016llx\",\"note\":", crash_signal, (unsigned long long)crash_pc); jstr(f, crash_note); fputc('}', f); }
    fprintf(f, "],\"notes\":[");
    for (unsigned i = 0; i < nnotes; i++) { fprintf(f, "%s", i ? "," : ""); jstr(f, notes[i]); }
    fprintf(f, "]}}\n");
    if (fclose(f) == 0) rename(tmp, summary_path);
}

_Noreturn void runtime_fail(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    pthread_mutex_lock(&log_lock);
    fprintf(stderr, "[napple %8.3f] FAIL: %s\n", since_start(), buf);
    pthread_mutex_unlock(&log_lock);
    trace_event("fail", "\"message\":\"%s\"", "see log");
    summary_write("fail", buf);
    filelog_drain();
    _exit(70);
}
