/* Napple rooms, the connection: one TCP connection per process to a napple-room server (net_room.h).
 *
 * A worker thread owns the connection: it dials (and redials every few seconds after a loss, asking for the address it had), reads and dispatches the
 * server's frames, flushes the outgoing queue, pings every 5 s and also serves the virtual LAN's own descriptors (net_room_vlan.c). Guest threads only
 * append to the outgoing queue (1 MiB; a packet that does not fit is dropped, as a congested LAN drops it), so a slow server never blocks a title.
 * Frames from the server are bounds-checked like any network input. */
#include "net_room.h"
#include "net_host.h"
#include "hz_result.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void log_line(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define HDR 8u
#define OUT_CAP (1u << 20)
#define PING_MS 5000
#define RETRY_MS 3000
#define DIAL_MS 5000
#define MAX_MEMBERS 256

typedef struct {
    uint32_t id, vip;
    char nick[33];
} Mem;

static struct {
    pthread_mutex_t lock;     /* configuration, membership, up / vip */
    pthread_mutex_t out_lock; /* the outgoing queue and the descriptor it writes to */
    pthread_cond_t cond;      /* signalled on every (re)join or failed first dial */
    int configured, started, start_result, vlan, fatal, dials;
    char host[256], room[65], password[65], nick[33];
    uint16_t port;
    uint32_t want_vip;
    /* connection */
    int fd, up;
    uint32_t member_id, vip, mask;
    Mem mem[MAX_MEMBERS];
    unsigned nmem;
    uint8_t *out;
    size_t out_len;
    uint8_t in[HDR + NR_MAX_BODY + 16];
    size_t in_len;
    /* worker */
    pthread_t th;
    int running, wake[2];
    int64_t next_ping, next_retry;
} R = {.lock = PTHREAD_MUTEX_INITIALIZER, .out_lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER, .fd = -1, .wake = {-1, -1}};

static void ip_text(uint32_t ip, char *b, size_t n) { snprintf(b, n, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255); }

/* ---- little-endian helpers ---------------------------------------------------------------------------------------------------------------------------- */
static void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void p32(uint8_t *p, uint32_t v) { p16(p, (uint16_t)v), p16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t g32(const uint8_t *p) { return g16(p) | (uint32_t)g16(p + 2) << 16; }
static void hdr(uint8_t *p, uint16_t type, uint32_t len) {
    p16(p, type), p16(p + 2, 0), p32(p + 4, len);
}
static size_t pstr(uint8_t *p, const char *s) {
    size_t n = strlen(s);
    if (n > 255) n = 255;
    p[0] = (uint8_t)n;
    memcpy(p + 1, s, n);
    return n + 1;
}

/* ---- configuration ------------------------------------------------------------------------------------------------------------------------------------ */
int nr_parse_room(const char *s, char *host, size_t hn, uint16_t *port, char *room, size_t rn) {
    char buf[512];
    if (!strcmp(s, "default") || !strncmp(s, "default/", 8)) snprintf(buf, sizeof buf, "%s%s", NR_DEFAULT_SERVER, s + 7); /* the public Napple server */
    else snprintf(buf, sizeof buf, "%s", s);
    char *slash = NULL;
    char *hp = buf;
    if (*hp == '[') { /* [v6]:port/room */
        char *e = strchr(hp, ']');
        if (!e) return -1;
        slash = strchr(e, '/');
    } else slash = strchr(hp, '/');
    if (slash) {
        *slash = 0;
        snprintf(room, rn, "%s", slash + 1);
    } else room[0] = 0;
    *port = NR_DEFAULT_PORT;
    char *colon = NULL;
    if (*hp == '[') {
        char *e = strchr(hp, ']');
        *e = 0;
        hp++;
        if (e[1] == ':') colon = e + 1;
    } else {
        colon = strrchr(hp, ':');
    }
    if (colon) {
        *colon = 0;
        long v = strtol(colon + 1, NULL, 10);
        if (v <= 0 || v > 65535) return -1;
        *port = (uint16_t)v;
    }
    if (!*hp) return -1;
    snprintf(host, hn, "%s", hp);
    return 0;
}

const char *net_room_nick(void) { return R.nick; }
int net_room_configured(void) { return __atomic_load_n(&R.configured, __ATOMIC_ACQUIRE); }
int net_room_up(void) { return __atomic_load_n(&R.up, __ATOMIC_ACQUIRE); }
int net_room_vlan_on(void) { return net_room_up() && R.vlan; }
uint32_t net_room_vip(void) {
    pthread_mutex_lock(&R.lock);
    uint32_t v = R.vip;
    pthread_mutex_unlock(&R.lock);
    return v;
}
uint32_t net_room_mask(void) {
    pthread_mutex_lock(&R.lock);
    uint32_t v = R.mask;
    pthread_mutex_unlock(&R.lock);
    return v;
}
int net_room_is_broadcast(uint32_t ip) {
    if (ip == 0xffffffffu) return 1;
    pthread_mutex_lock(&R.lock);
    int r = R.mask && ip == ((R.vip & R.mask) | ~R.mask);
    pthread_mutex_unlock(&R.lock);
    return r;
}
int net_room_is_vlan_dst(uint32_t ip) {
    if (ip == 0xffffffffu) return 1;
    pthread_mutex_lock(&R.lock);
    int r = R.mask && R.vip && (ip & R.mask) == (R.vip & R.mask) && ip != (R.vip & R.mask);
    pthread_mutex_unlock(&R.lock);
    return r;
}
int net_room_member_present(uint32_t vip) {
    pthread_mutex_lock(&R.lock);
    int r = 0;
    for (unsigned i = 0; i < R.nmem && !r; i++) r = R.mem[i].vip == vip;
    pthread_mutex_unlock(&R.lock);
    return r;
}

int net_room_net_info(OnlineNetInfo *out) {
    if (!net_room_configured() || !R.vlan) return 0;
    OnlineNetInfo host;
    nh_net_info(NULL, &host); /* DNS servers stay the Mac's: names still resolve through the host */
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&R.lock);
    if (R.up && R.vip) {
        out->connected = 1;
        out->kind = 2; /* wired: LAN play modes want a wired LAN */
        out->strength = 3;
        out->ip = R.vip;
        out->mask = R.mask;
        out->gateway = (R.vip & R.mask) | 1;
        out->dns1 = host.dns1;
        out->dns2 = host.dns2;
    }
    pthread_mutex_unlock(&R.lock);
    return 1;
}

/* ---- outgoing queue ----------------------------------------------------------------------------------------------------------------------------------- */
static void wake_worker(void) {
    if (R.wake[1] >= 0 && write(R.wake[1], "w", 1) < 0) {}
}
/* Writes what it can of the queue (out_lock held). -1 when the connection failed. */
static int flush_locked(void) {
    while (R.out_len && R.fd >= 0) {
        ssize_t k = send(R.fd, R.out, R.out_len, 0);
        if (k > 0) {
            memmove(R.out, R.out + k, R.out_len - (size_t)k);
            R.out_len -= (size_t)k;
            continue;
        }
        if (k < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        return -1;
    }
    return 0;
}
/* Appends one frame made of a header and up to two body parts. */
static int enqueue(uint16_t type, const void *a, size_t an, const void *b, size_t bn) {
    if (an + bn > NR_MAX_BODY) return -1;
    pthread_mutex_lock(&R.out_lock);
    int r = -1;
    if (R.fd >= 0 && R.up && R.out && R.out_len + HDR + an + bn <= OUT_CAP) {
        uint8_t *p = R.out + R.out_len;
        hdr(p, type, (uint32_t)(an + bn));
        if (an) memcpy(p + HDR, a, an);
        if (bn) memcpy(p + HDR + an, b, bn);
        int was_empty = R.out_len == 0;
        R.out_len += HDR + an + bn;
        r = 0;
        if (was_empty) flush_locked(); /* an error is noticed by the worker */
        if (R.out_len) wake_worker();
    }
    pthread_mutex_unlock(&R.out_lock);
    return r;
}
int net_room_send_frame(uint16_t type, const void *body, size_t n) { return enqueue(type, body, n, NULL, 0); }
int net_room_send_packet(uint32_t dst, uint8_t proto, uint8_t kind, uint16_t sport, uint16_t dport, const void *payload, size_t n) {
    uint8_t h[8 + NR_BODY_HDR];
    pthread_mutex_lock(&R.lock);
    uint32_t src = R.vip;
    pthread_mutex_unlock(&R.lock);
    p32(h, src), p32(h + 4, dst);
    h[8] = proto, h[9] = kind;
    p16(h + 10, sport), p16(h + 12, dport);
    return enqueue(NR_T_PACKET, h, sizeof h, payload, n);
}

/* ---- dialing ------------------------------------------------------------------------------------------------------------------------------------------ */
static int wait_fd(int fd, short ev, int ms) {
    struct pollfd pf = {fd, ev, 0};
    int k;
    do k = poll(&pf, 1, ms);
    while (k < 0 && errno == EINTR);
    return k > 0;
}
static int read_exact(int fd, uint8_t *p, size_t n, int64_t end) {
    size_t got = 0;
    while (got < n) {
        int64_t left = end - nh_now_ms();
        if (left <= 0 || !wait_fd(fd, POLLIN, (int)left)) return -1;
        ssize_t k = recv(fd, p + got, n - got, 0);
        if (k > 0) got += (size_t)k;
        else if (k == 0 || (errno != EAGAIN && errno != EINTR)) return -1;
    }
    return 0;
}
static int write_exact(int fd, const uint8_t *p, size_t n, int64_t end) {
    size_t done = 0;
    while (done < n) {
        ssize_t k = send(fd, p + done, n - done, 0);
        if (k > 0) {
            done += (size_t)k;
            continue;
        }
        if (k < 0 && (errno == EAGAIN || errno == EINTR)) {
            int64_t left = end - nh_now_ms();
            if (left <= 0 || !wait_fd(fd, POLLOUT, (int)left)) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}
static int connect_host(int64_t end) {
    char port[8];
    snprintf(port, sizeof port, "%u", R.port);
    struct addrinfo hints = {.ai_socktype = SOCK_STREAM, .ai_family = AF_UNSPEC}, *res = NULL;
    if (getaddrinfo(R.host, port, &hints, &res) != 0 || !res) return -1;
    int fd = -1;
    for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, SOCK_STREAM, 0);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        int ok = connect(fd, a->ai_addr, a->ai_addrlen) == 0;
        if (!ok && errno == EINPROGRESS) {
            int64_t left = end - nh_now_ms();
            int so = 0;
            socklen_t sl = sizeof so;
            ok = left > 0 && wait_fd(fd, POLLOUT, (int)left) && getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl) == 0 && !so;
        }
        if (!ok) close(fd), fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}
static void set_members(const uint8_t *p, size_t n, unsigned count) {
    R.nmem = 0;
    size_t off = 0;
    for (unsigned i = 0; i < count && off + 9 <= n; i++) {
        Mem m = {.id = g32(p + off), .vip = g32(p + off + 4)};
        size_t sl = p[off + 8];
        if (off + 9 + sl > n) break;
        memcpy(m.nick, p + off + 9, sl < 32 ? sl : 32);
        off += 9 + sl;
        if (R.nmem < MAX_MEMBERS) R.mem[R.nmem++] = m;
    }
}
/* One attempt: connect, HELLO, WELCOME. 0 joined; -1 failed (R.fatal set when retrying cannot help). */
static int dial(void) {
    int64_t end = nh_now_ms() + DIAL_MS;
    int fd = connect_host(end);
    if (fd < 0) return -1;
    uint8_t b[HDR + 8 + 3 * 256];
    size_t n = HDR;
    p32(b + n, NR_PROTO_VERSION), p32(b + n + 4, R.want_vip);
    n += 8;
    n += pstr(b + n, R.room);
    n += pstr(b + n, R.password);
    n += pstr(b + n, R.nick);
    hdr(b, NR_T_HELLO, (uint32_t)(n - HDR));
    uint8_t h[HDR];
    if (write_exact(fd, b, n, end) < 0 || read_exact(fd, h, HDR, end) < 0) {
        close(fd);
        return -1;
    }
    uint16_t type = g16(h);
    uint32_t len = g32(h + 4);
    uint8_t *body = len <= NR_MAX_BODY ? malloc(len ? len : 1) : NULL;
    if (!body || read_exact(fd, body, len, end) < 0) {
        free(body);
        close(fd);
        return -1;
    }
    int r = -1;
    if (type == NR_T_ERROR && len >= 5) {
        uint32_t code = g32(body);
        char msg[256] = {0};
        size_t ml = body[4] < len - 5 ? body[4] : len - 5;
        memcpy(msg, body + 5, ml);
        log_line("net", "room: server refused to join '%s': %s (code %u)", R.room, msg, code);
        if (code == 1 || code == 2 || code == 6) R.fatal = 1; /* version, password, name: retrying cannot help */
    } else if (type == NR_T_WELCOME && len >= 16) {
        pthread_mutex_lock(&R.lock);
        R.member_id = g32(body);
        R.vip = g32(body + 4);
        R.mask = g32(body + 8);
        set_members(body + 16, len - 16, g16(body + 14));
        R.want_vip = R.vip;
        unsigned nm = R.nmem;
        pthread_mutex_unlock(&R.lock);
        pthread_mutex_lock(&R.out_lock);
        R.fd = fd;
        R.out_len = 0;
        R.in_len = 0;
        __atomic_store_n(&R.up, 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&R.out_lock);
        char ip[20];
        ip_text(R.vip, ip, sizeof ip);
        log_line("net", "room: joined '%s' on %s:%u as %s (member %u, %u in the room, virtual LAN %s)", R.room, R.host, R.port, ip, R.member_id, nm,
                 R.vlan ? "on" : "off");
        r = 0;
    }
    free(body);
    if (r) close(fd);
    return r;
}
static void drop_connection(const char *why) {
    pthread_mutex_lock(&R.out_lock);
    int was = R.fd >= 0;
    if (R.fd >= 0) close(R.fd);
    R.fd = -1;
    R.out_len = 0;
    __atomic_store_n(&R.up, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&R.out_lock);
    pthread_mutex_lock(&R.lock);
    R.nmem = 0;
    pthread_mutex_unlock(&R.lock);
    if (!was) return;
    if (R.running) log_line("net", "room: connection to %s:%u lost (%s); retrying", R.host, R.port, why);
    else log_line("net", "room: left '%s'", R.room);
    nr_vlan_room_down();
    nr_ldn_room_down();
}

/* ---- inbound ------------------------------------------------------------------------------------------------------------------------------------------ */
static void member_join(const uint8_t *p, size_t n) {
    if (n < 9) return;
    Mem m = {.id = g32(p), .vip = g32(p + 4)};
    size_t sl = p[8] < n - 9 ? p[8] : n - 9;
    memcpy(m.nick, p + 9, sl < 32 ? sl : 32);
    pthread_mutex_lock(&R.lock);
    if (R.nmem < MAX_MEMBERS) R.mem[R.nmem++] = m;
    pthread_mutex_unlock(&R.lock);
    char ip[20];
    ip_text(m.vip, ip, sizeof ip);
    log_line("net", "room: '%s' joined as %s", m.nick, ip);
}
static void member_leave(const uint8_t *p, size_t n) {
    if (n < 8) return;
    uint32_t id = g32(p), vip = g32(p + 4);
    pthread_mutex_lock(&R.lock);
    for (unsigned i = 0; i < R.nmem; i++)
        if (R.mem[i].id == id) {
            R.mem[i] = R.mem[--R.nmem];
            break;
        }
    pthread_mutex_unlock(&R.lock);
    char ip[20];
    ip_text(vip, ip, sizeof ip);
    log_line("net", "room: %s left", ip);
    nr_vlan_member_left(vip);
    nr_ldn_member_left(vip);
}
static void frame_in(uint16_t type, const uint8_t *p, size_t n) {
    switch (type) {
        case NR_T_PACKET: {
            if (n < 8 + NR_BODY_HDR) return;
            uint32_t src = g32(p), dst = g32(p + 4);
            uint8_t proto = p[8], kind = p[9];
            uint16_t sport = g16(p + 10), dport = g16(p + 12);
            const uint8_t *pl = p + 8 + NR_BODY_HDR;
            size_t pn = n - 8 - NR_BODY_HDR;
            if (proto == NR_BODY_LDN) nr_ldn_packet_in(src, kind, pl, pn);
            else if (R.vlan) nr_vlan_packet_in(src, dst, proto, kind, sport, dport, pl, pn);
            break;
        }
        case NR_T_MEMBER_JOIN: member_join(p, n); break;
        case NR_T_MEMBER_LEAVE: member_leave(p, n); break;
        case NR_T_LDN_SCAN_RESULT: nr_ldn_scan_result(p, n); break;
        case NR_T_ERROR:
            log_line("net", "room: server error frame (code %u)", n >= 4 ? g32(p) : 0);
            break;
        default: break; /* PONG and anything newer */
    }
}
/* Reads what is there and dispatches whole frames. -1 when the connection ended or sent garbage. */
static int read_in(void) {
    for (;;) {
        size_t cap = sizeof R.in - R.in_len;
        ssize_t k = recv(R.fd, R.in + R.in_len, cap, 0);
        if (k == 0) return -1;
        if (k < 0) return errno == EAGAIN || errno == EINTR ? 0 : -1;
        R.in_len += (size_t)k;
        size_t off = 0;
        while (R.in_len - off >= HDR) {
            uint16_t type = g16(R.in + off), rsv = g16(R.in + off + 2);
            uint32_t len = g32(R.in + off + 4);
            if (rsv || len > NR_MAX_BODY) return -1;
            if (R.in_len - off < HDR + len) break;
            frame_in(type, R.in + off + HDR, len);
            off += HDR + len;
        }
        memmove(R.in, R.in + off, R.in_len - off);
        R.in_len -= off;
    }
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        if (!__atomic_load_n(&R.running, __ATOMIC_ACQUIRE)) break;
        int64_t now = nh_now_ms();
        if (R.fd < 0 && !R.fatal && now >= R.next_retry) {
            int ok = dial() == 0;
            pthread_mutex_lock(&R.lock);
            R.dials++;
            pthread_cond_broadcast(&R.cond);
            pthread_mutex_unlock(&R.lock);
            if (!ok) {
                R.next_retry = nh_now_ms() + RETRY_MS;
                if (R.dials == 1) log_line("net", "room: cannot reach %s:%u yet; retrying every %d s", R.host, R.port, RETRY_MS / 1000);
            } else R.next_ping = nh_now_ms() + PING_MS;
        }
        struct pollfd pf[2 + 256];
        int n = 0;
        pf[n++] = (struct pollfd){R.wake[0], POLLIN, 0};
        pthread_mutex_lock(&R.out_lock);
        int fd = R.fd;
        if (fd >= 0) pf[n++] = (struct pollfd){fd, (short)(POLLIN | (R.out_len ? POLLOUT : 0)), 0};
        pthread_mutex_unlock(&R.out_lock);
        int nv = nr_vlan_pollfds(pf + n, 256);
        int k = poll(pf, (nfds_t)(n + nv), 100);
        if (k < 0 && errno != EINTR) continue;
        if (pf[0].revents) {
            char b[64];
            while (read(R.wake[0], b, sizeof b) > 0) {}
        }
        if (fd >= 0 && pf[1].revents) {
            if ((pf[1].revents & (POLLIN | POLLHUP | POLLERR)) && read_in() < 0) {
                drop_connection("closed by the server");
                R.next_retry = nh_now_ms() + RETRY_MS;
            } else if (pf[1].revents & POLLOUT) {
                pthread_mutex_lock(&R.out_lock);
                int bad = flush_locked() < 0;
                pthread_mutex_unlock(&R.out_lock);
                if (bad) {
                    drop_connection("write failed");
                    R.next_retry = nh_now_ms() + RETRY_MS;
                }
            }
        }
        nr_vlan_poll_done(pf + n, nv);
        if (R.fd >= 0 && nh_now_ms() >= R.next_ping) {
            uint8_t tok[8] = {0};
            enqueue(NR_T_PING, tok, 8, NULL, 0);
            R.next_ping = nh_now_ms() + PING_MS;
        }
    }
    return NULL;
}

/* ---- start / stop ------------------------------------------------------------------------------------------------------------------------------------- */
static int begin(void) {
    R.out = malloc(OUT_CAP);
    if (!R.out || pipe(R.wake) != 0) return -1;
    fcntl(R.wake[0], F_SETFL, O_NONBLOCK), fcntl(R.wake[1], F_SETFL, O_NONBLOCK);
    fcntl(R.wake[0], F_SETFD, FD_CLOEXEC), fcntl(R.wake[1], F_SETFD, FD_CLOEXEC);
    __atomic_store_n(&R.configured, 1, __ATOMIC_RELEASE);
    R.running = 1;
    if (pthread_create(&R.th, NULL, worker, NULL) != 0) {
        R.running = 0;
        return -1;
    }
    /* wait for the first answer (or a failed attempt) so a title that checks nifm at boot sees the room */
    pthread_mutex_lock(&R.lock);
    int64_t end = nh_now_ms() + DIAL_MS + 500;
    while (!R.dials && nh_now_ms() < end) {
        struct timespec ts = {0, 50 * 1000 * 1000};
        pthread_mutex_unlock(&R.lock);
        nanosleep(&ts, NULL);
        pthread_mutex_lock(&R.lock);
    }
    pthread_mutex_unlock(&R.lock);
    return 0;
}

static pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;
int net_room_start_with(const char *server, const char *room, const char *password, const char *nick, int vlan) {
    pthread_mutex_lock(&start_lock);
    if (R.started) {
        int r = R.start_result;
        pthread_mutex_unlock(&start_lock);
        return r;
    }
    R.started = 1;
    R.start_result = -1;
    if (server && nr_parse_room(server, R.host, sizeof R.host, &R.port, R.room, sizeof R.room) == 0) {
        if (room) snprintf(R.room, sizeof R.room, "%s", room);
        snprintf(R.password, sizeof R.password, "%s", password ? password : "");
        const char *u = nick && *nick ? nick : getenv("USER");
        snprintf(R.nick, sizeof R.nick, "%s", u && *u ? u : "napple");
        R.vlan = vlan != 0;
        R.start_result = begin();
        if (R.start_result) log_line("net", "room: could not start the room client");
    } else log_line("net", "room: NAPPLE_ROOM '%s' is not <host>[:<port>][/<room>]", server ? server : "");
    pthread_mutex_unlock(&start_lock);
    return R.start_result;
}
int net_room_start(void) {
    const char *s = getenv("NAPPLE_ROOM");
    if (!s || !*s) return -1;
    const char *v = getenv("NAPPLE_VLAN");
    int vlan = 1;
    if (v && (!strcmp(v, "off") || !strcmp(v, "0"))) vlan = 0;
    else if (v && *v && strcmp(v, "on") && strcmp(v, "1")) {
        struct in_addr a;
        if (inet_pton(AF_INET, v, &a) == 1) R.want_vip = ntohl(a.s_addr);
        else log_line("net", "room: NAPPLE_VLAN '%s' is not on, off or an IPv4 address: ignored", v);
    }
    return net_room_start_with(s, NULL, getenv("NAPPLE_ROOM_PASSWORD"), getenv("NAPPLE_ROOM_NICK"), vlan);
}
void net_room_stop(void) {
    pthread_mutex_lock(&start_lock);
    if (R.started && R.running) {
        __atomic_store_n(&R.running, 0, __ATOMIC_RELEASE);
        wake_worker();
        pthread_join(R.th, NULL);
        drop_connection("stopped");
    }
    pthread_mutex_unlock(&start_lock);
}
