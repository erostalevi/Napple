/* online core: the provider registry, the NullProvider (a console with no network) and the plugin loader (portable).
 * The NullProvider is the previous offline behaviour of bsd, nifm, sfdnsres, ssl and friend moved behind the OnlineProvider table:
 * the errno and result decisions are unchanged (offline_core.c still owns the socket-table decisions). */
#include "online_provider.h"
#include "offline_core.h"
#include "hz_result.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(NAPPLE_ONLINE_NO_PLUGIN) && defined(__has_include)
#if __has_include(<dlfcn.h>)
#include <dlfcn.h>
#define ONLINE_HAVE_DLFCN 1
#endif
#endif
#include "../platform.h"

void log_line(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ---- NullProvider ---------------------------------------------------------------------------------------------------------- */
static BsdTable null_fds;
static int null_ready;
static pthread_mutex_t null_lock = PTHREAD_MUTEX_INITIALIZER;
static BsdTable *nt(void) {
    if (!null_ready) {
        bsd_init(&null_fds);
        null_ready = 1;
    }
    return &null_fds;
}
void online_null_reset(void) {
    pthread_mutex_lock(&null_lock);
    bsd_init(&null_fds);
    null_ready = 1;
    pthread_mutex_unlock(&null_lock);
}
static int null_op(BsdOp op, int h, int *err) {
    pthread_mutex_lock(&null_lock);
    int r = bsd_op(nt(), op, h, err);
    pthread_mutex_unlock(&null_lock);
    return r;
}
static int null_net_info(void *c, OnlineNetInfo *o) {
    (void)c;
    memset(o, 0, sizeof *o);
    return 0;
}
static uint32_t null_net_connect(void *c) {
    (void)c;
    return HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED;
}
static void null_net_release(void *c) { (void)c; }
static int null_open(void *c, int d, int t, int p, int *err) {
    (void)c;
    pthread_mutex_lock(&null_lock);
    int r = bsd_socket(nt(), d, t, p, err);
    pthread_mutex_unlock(&null_lock);
    return r;
}
static int null_close(void *c, int h, int *err) {
    (void)c;
    pthread_mutex_lock(&null_lock);
    int r = bsd_close(nt(), h, err);
    pthread_mutex_unlock(&null_lock);
    return r;
}
static int null_bind(void *c, int h, const OnlineAddr *a, int *err) {
    (void)c;
    (void)a;
    return null_op(BSD_OP_BIND, h, err);
}
static int null_listen(void *c, int h, int b, int *err) {
    (void)c;
    (void)b;
    return null_op(BSD_OP_LISTEN, h, err);
}
static int null_accept(void *c, int h, OnlineAddr *peer, int *err) {
    (void)c;
    (void)peer;
    return null_op(BSD_OP_ACCEPT, h, err);
}
static int null_connect(void *c, int h, const OnlineAddr *a, int *err) {
    (void)c;
    (void)a;
    return null_op(BSD_OP_CONNECT, h, err);
}
static int64_t null_send(void *c, int h, const void *b, size_t n, int fl, const OnlineAddr *to, int *err) {
    (void)c;
    (void)b;
    (void)n;
    (void)fl;
    return null_op(to ? BSD_OP_SENDTO : BSD_OP_SEND, h, err);
}
static int64_t null_recv(void *c, int h, void *b, size_t n, int fl, OnlineAddr *from, int *err) {
    (void)c;
    (void)b;
    (void)n;
    (void)fl;
    return null_op(from ? BSD_OP_RECVFROM : BSD_OP_RECV, h, err);
}
static int null_shutdown_sock(void *c, int h, int how, int *err) {
    (void)c;
    (void)how;
    return null_op(BSD_OP_SHUTDOWN, h, err);
}
static int null_getsockname(void *c, int h, OnlineAddr *o, int *err) {
    (void)c;
    int r = null_op(BSD_OP_GETSOCKNAME, h, err);
    if (r == 0) {
        memset(o, 0, sizeof *o);
        o->family = BSD_AF_INET;
    }
    return r;
}
static int null_getpeername(void *c, int h, OnlineAddr *o, int *err) {
    (void)c;
    (void)o;
    return null_op(BSD_OP_GETPEERNAME, h, err);
}
static int null_setsockopt(void *c, int h, int lv, int opt, const void *v, size_t n, int *err) {
    (void)c;
    (void)lv;
    (void)opt;
    (void)v;
    (void)n;
    return null_op(BSD_OP_SETSOCKOPT, h, err);
}
static int null_getsockopt(void *c, int h, int lv, int opt, void *v, size_t *n, int *err) {
    (void)c;
    (void)lv;
    (void)opt;
    if (v && n) memset(v, 0, *n); /* option values read as zero, as the stub always did */
    return null_op(BSD_OP_GETSOCKOPT, h, err);
}
static int null_nonblock(void *c, int h, int on, int *err) {
    (void)c;
    (void)on;
    return null_op(BSD_OP_FCNTL, h, err);
}
static int null_poll(void *c, OnlinePollFd *fds, unsigned n, int timeout_ms, int *err) {
    (void)c;
    (void)timeout_ms;
    int ready = 0;
    *err = 0;
    pthread_mutex_lock(&null_lock);
    for (unsigned i = 0; i < n; i++) {
        fds[i].revents = 0;
        if (fds[i].fd >= 0 && !bsd_lookup(nt(), fds[i].fd)) {
            fds[i].revents = ONLINE_POLLNVAL;
            ready++;
        }
    }
    pthread_mutex_unlock(&null_lock);
    return ready; /* nothing ever becomes ready: the call reports a timeout */
}
static int null_dns(void *c, const char *host, const char *service, int family, OnlineResolved *out) {
    (void)c;
    (void)host;
    (void)service;
    (void)family;
    memset(out, 0, sizeof *out);
    return HZ_EAI_AGAIN; /* no reachable resolver */
}
static int null_dns_reverse(void *c, const OnlineAddr *a, char *host, size_t n) {
    (void)c;
    (void)a;
    (void)host;
    (void)n;
    return HZ_EAI_AGAIN;
}
static void *null_tls_new(void *c, int s) {
    (void)c;
    (void)s;
    return NULL;
}
static uint32_t null_friend_count(void *c) {
    (void)c;
    return 0;
}
static uint32_t null_friend_list(void *c, uint32_t off, OnlineFriend *o, uint32_t max) {
    (void)c;
    (void)off;
    (void)o;
    (void)max;
    return 0;
}
static void null_presence(void *c, const void *r, size_t n) {
    (void)c;
    (void)r;
    (void)n;
}
static int null_nsd(void *c, const char *in, char *out, size_t sz) {
    (void)c;
    if (!sz) return -1;
    size_t n = strlen(in);
    if (n > sz - 1) n = sz - 1;
    memcpy(out, in, n);
    out[n] = 0;
    return 0; /* names resolve to themselves; nothing is ever contacted */
}

static const OnlineProvider null_provider = {
    .abi_version = ONLINE_ABI_VERSION,
    .struct_size = sizeof(OnlineProvider),
    .name = "null (offline)",
    .caps = 0,
    .net_info = null_net_info,
    .net_connect = null_net_connect,
    .net_release = null_net_release,
    .sock_open = null_open,
    .sock_close = null_close,
    .sock_bind = null_bind,
    .sock_listen = null_listen,
    .sock_accept = null_accept,
    .sock_connect = null_connect,
    .sock_send = null_send,
    .sock_recv = null_recv,
    .sock_shutdown = null_shutdown_sock,
    .sock_getsockname = null_getsockname,
    .sock_getpeername = null_getpeername,
    .sock_setsockopt = null_setsockopt,
    .sock_getsockopt = null_getsockopt,
    .sock_set_nonblock = null_nonblock,
    .sock_poll = null_poll,
    .dns_resolve = null_dns,
    .dns_reverse = null_dns_reverse,
    .tls_new = null_tls_new,
    .friend_count = null_friend_count,
    .friend_list = null_friend_list,
    .presence_update = null_presence,
    .nsd_resolve = null_nsd,
};
const OnlineProvider *online_null_provider(void) { return &null_provider; }

/* ---- registry ------------------------------------------------------------------------------------------------------------ */
static const OnlineProvider *active;
static int env_loaded;
static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;

int online_provider_check(const OnlineProvider *p) {
    if (!p || p->abi_version != ONLINE_ABI_VERSION) return -1;
    /* struct_size must cover at least the fields up to and including caps/ctx, or the table is unusable. */
    if (p->struct_size < offsetof(OnlineProvider, shutdown) + sizeof p->shutdown) return -2;
    if (p->init && p->init(p->ctx) != 0) return -3;
    return 0;
}

#if ONLINE_HAVE_DLFCN && !NAPPLE_IOS
static const OnlineProvider *load_plugin(const char *path) {
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        log_line("online", "provider '%s' cannot be loaded (%s): using the offline provider", path, dlerror());
        return NULL;
    }
    const OnlineProvider *(*entry)(void) = (const OnlineProvider *(*)(void))dlsym(lib, ONLINE_PLUGIN_SYMBOL);
    const OnlineProvider *p = entry ? entry() : NULL;
    int rc = online_provider_check(p);
    if (rc) {
        log_line("online", "provider '%s' rejected (%s, code %d): using the offline provider", path, entry ? "bad table" : "no " ONLINE_PLUGIN_SYMBOL, rc);
        dlclose(lib);
        return NULL;
    }
    log_line("online", "provider '%s' loaded from %s (caps 0x%x)", p->name ? p->name : "?", path, p->caps);
    return p;
}
#else
static const OnlineProvider *load_plugin(const char *path) {
    log_line("online", "provider '%s' ignored: this build cannot load provider modules (iOS has no runtime loading)", path);
    return NULL;
}
#endif

const OnlineProvider *online_provider_install(const OnlineProvider *p) {
    pthread_mutex_lock(&reg_lock);
    const OnlineProvider *old = active ? active : &null_provider;
    active = p; /* NULL = back to the NullProvider */
    __atomic_store_n(&env_loaded, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&reg_lock);
    return old;
}

void online_provider_reset(void) {
    pthread_mutex_lock(&reg_lock);
    active = NULL;
    __atomic_store_n(&env_loaded, 0, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&reg_lock);
}

static pthread_mutex_t load_lock = PTHREAD_MUTEX_INITIALIZER;
const OnlineProvider *online_provider(void) {
    if (!__atomic_load_n(&env_loaded, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&load_lock); /* one loader at a time; the registry lock is not held while the module runs, so its init may call back */
        if (!__atomic_load_n(&env_loaded, __ATOMIC_ACQUIRE)) {
            const char *path = getenv(ONLINE_ENV_PROVIDER);
            const OnlineProvider *p = path && *path ? load_plugin(path) : NULL;
            pthread_mutex_lock(&reg_lock);
            if (p && !active) active = p;
            pthread_mutex_unlock(&reg_lock);
            __atomic_store_n(&env_loaded, 1, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&load_lock);
    }
    pthread_mutex_lock(&reg_lock);
    const OnlineProvider *p = active ? active : &null_provider;
    pthread_mutex_unlock(&reg_lock);
    return p;
}
