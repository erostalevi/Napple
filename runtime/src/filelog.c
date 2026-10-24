/* filelog.c - persistent, size-capped log for sessions nobody watches (apps started with `open`, Finder, LaunchServices).
 *
 * When stderr is not a terminal and not already a regular file (typically /dev/null under LaunchServices), fd 2 is replaced by a pipe; a reader
 * thread filters what arrives and appends it to ~/Library/Logs/Napple/<titleid>.log, rotating at 5 MB across 3 files (.log, .log.1, .log.2).
 * Everything the process prints to stderr (runtime LOG, gfx warnings, the crash report written with write(2)) goes through the pipe, so no
 * call site changed. Default level is "warnings and runtime events": gfx `debug:`/`info:` lines, the per-read filesystem trace and exact
 * repeats of a line (the settings line prints every frame of a resize) are dropped or collapsed. NAPPLE_FILE_LOG=all keeps everything,
 * NAPPLE_FILE_LOG=0 disables the file. The log carries no key material: the runtime never prints keys.
 * filelog_drain() must be called before an abrupt _exit() so the last lines (the crash report) reach the file. */
#include "napple.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG_CAP (5u << 20)
#define LOG_FILES 3
#define FLUSH_MARK "\x01" "FLUSH\n"

static int g_rd = -1, g_wr = -1, g_active, g_keep_all;
static char g_path[1100];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_have_path;
static _Atomic unsigned g_flush_seen, g_flush_sent;
static FILE *g_f;
static size_t g_size;

static void mkdirs(const char *path) {
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
}
static void rotate(void) {
    if (g_f) { fclose(g_f); g_f = NULL; }
    char a[1200], b[1200];
    for (int i = LOG_FILES - 1; i >= 1; i--) {      /* .log.1 -> .log.2, .log -> .log.1 */
        if (i > 1) snprintf(a, sizeof a, "%s.%d", g_path, i - 1); else snprintf(a, sizeof a, "%s", g_path);
        snprintf(b, sizeof b, "%s.%d", g_path, i);
        rename(a, b);
    }
}
static void open_file(void) {
    struct stat st;
    if (stat(g_path, &st) == 0 && st.st_size > 0) {          /* a new session: keep the previous one as .1 */
        rotate();
    }
    g_f = fopen(g_path, "w"); g_size = 0;
}
static void emit(const char *line, size_t n) {
    if (!g_f) return;
    if (g_size + n > LOG_CAP) { rotate(); g_f = fopen(g_path, "w"); g_size = 0; if (!g_f) return; }
    fwrite(line, 1, n, g_f); g_size += n;
}
static const char *body(const char *l) {              /* the text after the "[napple  12.345] " prefix */
    if (!strncmp(l, "[napple ", 8)) { const char *e = strchr(l, ']'); if (e) return e + 2; }
    return l;
}
static int keep(const char *l) {
    if (g_keep_all) return 1;
    if (!strncmp(l, "[napple-gfx] debug:", 19) || !strncmp(l, "[napple-gfx] info:", 18)) return 0;
    if (strstr(l, "fs: RomFS read offset")) return 0;
    return 1;
}

static void *reader(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_mu);
    while (!g_have_path) pthread_cond_wait(&g_cv, &g_mu);       /* lines arriving before the title is known stay in the pipe (64 KiB) ... */
    pthread_mutex_unlock(&g_mu);
    open_file();
    char buf[8192], line[4096], last[4096] = ""; size_t ll = 0; unsigned repeats = 0;
    for (;;) {
        ssize_t n = read(g_rd, buf, sizeof buf);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
        for (ssize_t i = 0; i < n; i++) {
            char ch = buf[i];
            if (ll < sizeof line - 1) line[ll++] = ch;
            if (ch != '\n') continue;
            line[ll] = 0;
            if (!strcmp(line, FLUSH_MARK)) {
                if (repeats) { char r[96]; int k = snprintf(r, sizeof r, "[napple] (previous line repeated %u more times)\n", repeats); emit(r, (size_t)k); repeats = 0; last[0] = 0; }
                if (g_f) fflush(g_f);
                atomic_fetch_add(&g_flush_seen, 1);
            } else if (keep(line)) {
                const char *b = body(line);
                if (!strcmp(b, last)) repeats++;
                else {
                    if (repeats) { char r[96]; int k = snprintf(r, sizeof r, "[napple] (previous line repeated %u more times)\n", repeats); emit(r, (size_t)k); repeats = 0; }
                    emit(line, ll); snprintf(last, sizeof last, "%s", b);
                }
            }
            ll = 0;
        }
        if (g_f && ll == 0) fflush(g_f);
    }
    return NULL;
}

/* Early in main: decide, and take over fd 2 when the session is unattended. */
void filelog_start(void) {
    const char *e = getenv("NAPPLE_FILE_LOG");
    if (e && !strcmp(e, "0")) return;
    struct stat st;
    if (fstat(2, &st) != 0) return;
    if (isatty(2) || S_ISREG(st.st_mode)) return;               /* a terminal, or someone already redirects to a file */
    int p[2];
    if (pipe(p) != 0) return;
    g_keep_all = e && !strcmp(e, "all");
    g_rd = p[0]; g_wr = p[1];
    fcntl(g_wr, F_SETFD, FD_CLOEXEC); fcntl(g_rd, F_SETFD, FD_CLOEXEC);
    if (dup2(g_wr, 2) < 0) { close(p[0]); close(p[1]); return; }
    pthread_t t;
    if (pthread_create(&t, NULL, reader, NULL) != 0) return;
    pthread_detach(t);
    g_active = 1;
    atexit(filelog_drain);
}

/* Once the title is known: ~/Library/Logs/Napple/<titleid>.log */
void filelog_set_title(const char *title_id) {
    if (!g_active) return;
    const char *home = getenv("HOME");
    snprintf(g_path, sizeof g_path, "%s/Library/Logs/Napple/%s.log", home ? home : ".", title_id && *title_id ? title_id : "unknown");
    mkdirs(g_path);
    pthread_mutex_lock(&g_mu); g_have_path = 1; pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mu);
}

/* Waits (bounded) until everything printed so far is in the file. Safe in a signal handler: write(2), atomics, nanosleep. */
void filelog_drain(void) {
    if (!g_active || !g_have_path) return;
    unsigned want = atomic_fetch_add(&g_flush_sent, 1) + 1;
    if (write(g_wr, FLUSH_MARK, sizeof FLUSH_MARK - 1) < 0) return;
    for (int i = 0; i < 400 && atomic_load(&g_flush_seen) < want; i++) { struct timespec ts = { 0, 5 * 1000 * 1000 }; nanosleep(&ts, NULL); }
}
