/* Napple rooms end to end: a real napple-room server (crates/napple-room, started from ../target/{debug,release}/napple-room or NAPPLE_ROOM_SERVER_BIN)
 * and two consoles, A (this process) and B (a forked child), each with its own room client and host provider, on one Mac, as two players on two
 * machines would be. Checked: nifm reports the virtual interface; both bind the same UDP port (49152, as MK8D's LAN mode does); broadcasts reach the
 * other console and loop back to the sender; unicast both ways with the virtual source; an unbound socket sends (implicit bind); a connected UDP
 * socket; TCP connect / accept / data both ways / EOF with virtual peer names, and a refused connect; LDN scan / join / node list / advertise data /
 * leave / destroy through the room; a member that quits leaves the host's network. */
#include "check.h"
#include "hz_result.h"
#include "../src/net/net_host.h"
#include "../src/net/net_room.h"
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#define LCID 0x0100152000022000ull
#define SOL 0xffff
#define SO_RCVTIMEO_BSD 0x1006
#define SO_BROADCAST_BSD 0x20
#define SO_REUSEADDR_BSD 0x4

static int to_b[2], to_a[2];
static void sig(int fd) {
    char c = 1;
    if (write(fd, &c, 1) != 1) {}
}
static int await(int fd) {
    char c;
    struct pollfd p = {fd, POLLIN, 0};
    return poll(&p, 1, 15000) == 1 && read(fd, &c, 1) == 1;
}
static int notes;
static void note(void *arg) { __atomic_add_fetch((int *)arg, 1, __ATOMIC_SEQ_CST); }
static int wait_notes(int at_least) {
    for (int i = 0; i < 300; i++) {
        if (__atomic_load_n(&notes, __ATOMIC_SEQ_CST) >= at_least) return 1;
        usleep(10000);
    }
    return 0;
}
static OnlineAddr ip4(uint32_t ip, uint16_t port) {
    OnlineAddr a = {.family = 2, .port = port};
    a.addr[0] = (uint8_t)(ip >> 24), a.addr[1] = (uint8_t)(ip >> 16), a.addr[2] = (uint8_t)(ip >> 8), a.addr[3] = (uint8_t)ip;
    return a;
}
static uint32_t ip_of(const OnlineAddr *a) { return (uint32_t)a->addr[0] << 24 | (uint32_t)a->addr[1] << 16 | (uint32_t)a->addr[2] << 8 | a->addr[3]; }
static int udp_socket(const OnlineProvider *P, uint16_t port) {
    int err, one = 1, s = P->sock_open(P->ctx, 2, 2, 0, &err);
    int64_t tv[2] = {3, 0};
    P->sock_setsockopt(P->ctx, s, SOL, SO_BROADCAST_BSD, &one, 4, &err);
    P->sock_setsockopt(P->ctx, s, SOL, SO_REUSEADDR_BSD, &one, 4, &err);
    P->sock_setsockopt(P->ctx, s, SOL, SO_RCVTIMEO_BSD, tv, sizeof tv, &err);
    if (port) {
        OnlineAddr any = {.family = 2, .port = port};
        if (P->sock_bind(P->ctx, s, &any, &err) != 0) return -1;
    }
    return s;
}
static int64_t recv_from(const OnlineProvider *P, int s, char *buf, size_t n, OnlineAddr *from) {
    int err;
    int64_t k = P->sock_recv(P->ctx, s, buf, n - 1, 0, from, &err);
    if (k >= 0) buf[k] = 0;
    return k;
}

/* ---- console B (the child): answers A's steps; its own checks are reported through the exit status ---------------------------------------------- */
static int console_b(const char *server, const char *room) {
    close(to_b[1]), close(to_a[0]);
    await(to_b[0]); /* 0: A joined first */
    net_room_start_with(server, room, "pw", "bob", 1);
    OnlineProvider *P = net_host_instance_new();
    for (int i = 0; i < 300 && !net_room_up(); i++) usleep(10000);
    CHECK(net_room_up(), "B: joined the room");
    sig(to_a[1]); /* 1: joined */
    int err;
    int u = udp_socket(P, 49152);
    CHECK(u >= 0, "B: binds UDP 49152 too (same Mac, own virtual address)");
    await(to_b[0]); /* 2: A bound */
    const char *hello = "anyone on the LAN?";
    OnlineAddr bc = ip4(0x0a0dffff, 49152), from;
    CHECK(P->sock_send(P->ctx, u, hello, strlen(hello), 0, &bc, &err) == (int64_t)strlen(hello), "B: subnet broadcast sent");
    char buf[256];
    CHECK(recv_from(P, u, buf, sizeof buf, &from) > 0 && !strcmp(buf, hello) && ip_of(&from) == net_room_vip(), "B: its own broadcast loops back");
    CHECK(recv_from(P, u, buf, sizeof buf, &from) > 0 && !strcmp(buf, "hi B") && from.port == 49152 && ip_of(&from) == 0x0a0d0002, "B: A's unicast reply, from A's virtual address");
    /* an unbound socket: implicit bind to an ephemeral virtual port */
    int u2 = udp_socket(P, 0);
    OnlineAddr a49152 = ip4(0x0a0d0002, 49152);
    P->sock_send(P->ctx, u2, "implicit", 8, 0, &a49152, &err);
    OnlineAddr me;
    CHECK(P->sock_getsockname(P->ctx, u2, &me, &err) == 0 && me.port >= 50000 && me.port < 60000, "B: implicitly bound to a virtual port");
    sig(to_a[1]); /* 3: sent */
    /* TCP */
    await(to_b[0]); /* 4: A listens on 7000 */
    int t = P->sock_open(P->ctx, 2, 1, 0, &err);
    OnlineAddr a7000 = ip4(0x0a0d0002, 7000);
    CHECK(P->sock_connect(P->ctx, t, &a7000, &err) == 0, "B: TCP connect to A over the room (err %d)", err);
    OnlineAddr pn, sn;
    CHECK(P->sock_getpeername(P->ctx, t, &pn, &err) == 0 && ip_of(&pn) == 0x0a0d0002 && pn.port == 7000, "B: peer name is A's virtual address");
    CHECK(P->sock_getsockname(P->ctx, t, &sn, &err) == 0 && ip_of(&sn) == net_room_vip(), "B: own name is the virtual address");
    static uint8_t big[100000];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 7);
    CHECK(P->sock_send(P->ctx, t, big, sizeof big, 0, NULL, &err) == (int64_t)sizeof big, "B: 100000 bytes sent");
    static uint8_t back[100000];
    size_t got = 0;
    while (got < sizeof back) {
        int64_t k = P->sock_recv(P->ctx, t, back + got, sizeof back - got, 0, NULL, &err);
        if (k <= 0) break;
        got += (size_t)k;
    }
    CHECK(got == sizeof back && !memcmp(back, big, sizeof big), "B: the echo came back intact (%zu bytes)", got);
    P->sock_shutdown(P->ctx, t, 1, &err);
    CHECK(P->sock_recv(P->ctx, t, back, 16, 0, NULL, &err) == 0, "B: EOF after both sides closed");
    int t2 = P->sock_open(P->ctx, 2, 1, 0, &err);
    OnlineAddr a7001 = ip4(0x0a0d0002, 7001);
    CHECK(P->sock_connect(P->ctx, t2, &a7001, &err) == -1 && err == HZ_ECONNREFUSED, "B: nothing listens on 7001: refused (err %d)", err);
    sig(to_a[1]); /* 5: TCP done */
    /* LDN */
    await(to_b[0]); /* 6: A hosts */
    OnlineLdnNetwork nets[8];
    int n = P->ldn_scan(P->ctx, LCID, nets, 8);
    CHECK(n == 1 && nets[0].ipv4 == 0x0a0d0002 && nets[0].node_count == 1 && !strcmp(nets[0].host_name, "alice-host"), "B: scan finds A's network at A's address");
    CHECK(P->ldn_scan(P->ctx, LCID + 1, nets + 1, 4) == 0, "B: not under another title");
    CHECK(P->ldn_connect(P->ctx, &nets[0]) == 0, "B: joins A's network");
    OnlineLdnNode nodes[8];
    uint32_t self = 9;
    CHECK(P->ldn_get_nodes(P->ctx, nodes, 8, &self) == 8 && self == 1 && nodes[0].ipv4 == 0x0a0d0002 && nodes[1].ipv4 == net_room_vip() && nodes[1].connected,
          "B: node 0 is A, node 1 is B, at virtual addresses");
    sig(to_a[1]); /* 7: joined */
    await(to_b[0]); /* 8: A changed its advertise data */
    OnlineLdnNetwork cur;
    uint16_t reason = 99;
    int ok = 0;
    for (int i = 0; i < 200 && !ok; i++, usleep(10000))
        ok = P->ldn_get_network(P->ctx, &cur, &reason) == 0 && cur.advertise_len == 5 && !memcmp(cur.advertise, "round", 5);
    CHECK(ok, "B: the new advertise data arrived");
    P->ldn_disconnect(P->ctx);
    sig(to_a[1]); /* 9: left */
    await(to_b[0]); /* 10: rejoin */
    CHECK(P->ldn_connect(P->ctx, &nets[0]) == 0, "B: joins again");
    sig(to_a[1]); /* 11: joined again */
    await(to_b[0]); /* 12: A destroyed it */
    ok = 0;
    for (int i = 0; i < 300 && !ok; i++, usleep(10000)) ok = P->ldn_get_network(P->ctx, &cur, &reason) != 0;
    CHECK(ok && reason == 3, "B: the network ended, reason DestroyedByUser (%u)", reason);
    await(to_b[0]); /* 12b: A hosts a new network */
    n = P->ldn_scan(P->ctx, LCID, nets, 8);
    CHECK(n == 1 && P->ldn_connect(P->ctx, &nets[0]) == 0, "B: joins A's new network");
    sig(to_a[1]); /* 13: B quits now (while a member of A's new network) */
    printf("test_net_room (console B): %d checks, %d failed\n", g_checked, g_failed);
    fflush(stdout);
    _exit(g_failed ? 1 : 0);
}

static pid_t start_server(const char *bin, int port) {
    char listen[64];
    snprintf(listen, sizeof listen, "127.0.0.1:%d", port);
    char *argv[] = {(char *)bin, "serve", "--listen", listen, "--quiet", NULL};
    pid_t pid;
    if (posix_spawn(&pid, bin, NULL, NULL, argv, environ) != 0) return -1;
    return pid;
}

int main(void) {
    alarm(120); /* a hang fails the test (and its child) instead of blocking the suite */
    const char *bin = getenv("NAPPLE_ROOM_SERVER_BIN");
    const char *cands[] = {bin, "../target/debug/napple-room", "../target/release/napple-room"};
    bin = NULL;
    for (unsigned i = 0; i < 3 && !bin; i++)
        if (cands[i] && access(cands[i], X_OK) == 0) bin = cands[i];
    if (!bin) {
        printf("test_net_room: no napple-room server binary (cargo build -p napple-room, or set NAPPLE_ROOM_SERVER_BIN): FAIL\n");
        return 1;
    }
    int port = 41000 + (int)(getpid() % 9000);
    pid_t srv = start_server(bin, port);
    CHECK(srv > 0, "server started (%s)", bin);
    char server[64], room[64];
    snprintf(server, sizeof server, "127.0.0.1:%d", port);
    snprintf(room, sizeof room, "test-%d", (int)getpid());
    usleep(300000);

    /* B is forked before A has any thread or room connection; A joins first (10.13.0.2), then B (10.13.0.3) */
    if (pipe(to_b) || pipe(to_a)) return 1;
    fflush(stdout);
    pid_t b = fork();
    if (b == 0) return console_b(server, room);
    close(to_b[0]), close(to_a[1]);
    CHECK(net_room_start_with(server, room, "pw", "alice", 1) == 0, "A: room client started");
    for (int i = 0; i < 300 && !net_room_up(); i++) usleep(10000);
    CHECK(net_room_up() && net_room_vip() == 0x0a0d0002 && net_room_mask() == 0xffff0000, "A: joined as 10.13.0.2/16");
    sig(to_b[1]); /* 0 */
    OnlineProvider *P = net_host_instance_new();
    CHECK(P != NULL, "A: provider");
    CHECK(await(to_a[0]), "B joined");

    OnlineNetInfo ni;
    CHECK(P->net_info(P->ctx, &ni) == 0 && ni.connected && ni.kind == 2 && ni.ip == 0x0a0d0002 && ni.mask == 0xffff0000 && ni.gateway == 0x0a0d0001,
          "A: nifm reports the virtual interface (wired, 10.13.0.2/16, gateway .1)");
    CHECK(P->net_connect(P->ctx) == 0, "A: nifm connects");

    /* UDP */
    int err, u = udp_socket(P, 49152);
    CHECK(u >= 0, "A: binds UDP 49152");
    OnlineAddr nm;
    CHECK(P->sock_getsockname(P->ctx, u, &nm, &err) == 0 && nm.port == 49152 && ip_of(&nm) == 0, "A: getsockname is 0.0.0.0:49152");
    sig(to_b[1]); /* 2 */
    char buf[256];
    OnlineAddr from;
    CHECK(recv_from(P, u, buf, sizeof buf, &from) > 0 && !strcmp(buf, "anyone on the LAN?") && ip_of(&from) == 0x0a0d0003 && from.port == 49152,
          "A: B's broadcast arrives from 10.13.0.3:49152");
    OnlineAddr bto = from;
    CHECK(P->sock_send(P->ctx, u, "hi B", 4, 0, &bto, &err) == 4, "A: unicast reply");
    CHECK(await(to_a[0]), "B: implicit send done");
    CHECK(recv_from(P, u, buf, sizeof buf, &from) > 0 && !strcmp(buf, "implicit") && ip_of(&from) == 0x0a0d0003 && from.port >= 50000, "A: datagram from B's implicit port");
    /* a connected UDP socket only hears its peer */
    int uc = udp_socket(P, 0);
    OnlineAddr bimp = from;
    CHECK(P->sock_connect(P->ctx, uc, &bimp, &err) == 0, "A: UDP connect to B's port");
    CHECK(P->sock_send(P->ctx, uc, "c", 1, 0, NULL, &err) == 1, "A: send on the connected socket");
    OnlineAddr pn;
    CHECK(P->sock_getpeername(P->ctx, uc, &pn, &err) == 0 && ip_of(&pn) == 0x0a0d0003, "A: its peer is B");

    /* TCP: listen, accept, echo */
    int l = P->sock_open(P->ctx, 2, 1, 0, &err);
    OnlineAddr l7000 = {.family = 2, .port = 7000};
    CHECK(P->sock_bind(P->ctx, l, &l7000, &err) == 0 && P->sock_listen(P->ctx, l, 4, &err) == 0, "A: listens on TCP 7000");
    sig(to_b[1]); /* 4 */
    OnlineAddr peer;
    int c = P->sock_accept(P->ctx, l, &peer, &err);
    CHECK(c >= 0 && ip_of(&peer) == 0x0a0d0003, "A: accepts B (peer %u.%u.%u.%u)", peer.addr[0], peer.addr[1], peer.addr[2], peer.addr[3]);
    static uint8_t data[100000];
    size_t got = 0;
    while (got < sizeof data) {
        int64_t k = P->sock_recv(P->ctx, c, data + got, sizeof data - got, 0, NULL, &err);
        if (k <= 0) break;
        got += (size_t)k;
    }
    CHECK(got == sizeof data && data[12345] == (uint8_t)(12345 * 7), "A: 100000 bytes received");
    CHECK(P->sock_send(P->ctx, c, data, got, 0, NULL, &err) == (int64_t)got, "A: echoed");
    CHECK(P->sock_recv(P->ctx, c, data, 16, 0, NULL, &err) == 0, "A: B half-closed: EOF");
    P->sock_close(P->ctx, c, &err);
    CHECK(await(to_a[0]), "B: TCP done");

    /* LDN */
    P->ldn_set_notify(P->ctx, note, &notes);
    OnlineLdnNetwork cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.local_communication_id = LCID;
    cfg.node_count_max = 4;
    cfg.local_communication_version = 2;
    strcpy(cfg.host_name, "alice-host");
    CHECK(P->ldn_create(P->ctx, &cfg) == 0, "A: hosts an LDN network in the room");
    OnlineLdnNetwork cur;
    uint16_t reason;
    CHECK(P->ldn_get_network(P->ctx, &cur, &reason) == 0 && cur.ipv4 == 0x0a0d0002 && cur.node_count == 1, "A: node 0 at its virtual address");
    usleep(100000);
    sig(to_b[1]); /* 6 */
    CHECK(await(to_a[0]), "B joined the network");
    CHECK(wait_notes(1), "A: notified of the station");
    OnlineLdnNode nodes[8];
    uint32_t self;
    CHECK(P->ldn_get_nodes(P->ctx, nodes, 8, &self) == 8 && self == 0 && nodes[1].connected && nodes[1].ipv4 == 0x0a0d0003 && !strcmp(nodes[1].user_name, "bob"),
          "A: node 1 is bob at 10.13.0.3");
    CHECK(P->ldn_set_advertise(P->ctx, "round", 5) == 0, "A: new advertise data");
    sig(to_b[1]); /* 8 */
    CHECK(await(to_a[0]), "B left");
    int ok = 0;
    for (int i = 0; i < 200 && !ok; i++, usleep(10000)) ok = P->ldn_get_network(P->ctx, &cur, &reason) == 0 && cur.node_count == 1;
    CHECK(ok, "A: back to one node after B's disconnect");
    sig(to_b[1]); /* 10 */
    CHECK(await(to_a[0]), "B joined again");
    P->ldn_destroy(P->ctx);
    CHECK(P->ldn_get_network(P->ctx, &cur, &reason) != 0, "A: destroyed");
    sig(to_b[1]); /* 12 */
    usleep(300000);
    CHECK(P->ldn_create(P->ctx, &cfg) == 0, "A: hosts again");
    usleep(100000);
    sig(to_b[1]); /* 12b */
    CHECK(await(to_a[0]), "B about to quit");
    ok = 0;
    for (int i = 0; i < 200 && !ok; i++, usleep(10000)) ok = P->ldn_get_network(P->ctx, &cur, &reason) == 0 && cur.node_count == 2;
    CHECK(ok, "A: B is a member of the new network");
    int st = 0;
    CHECK(waitpid(b, &st, 0) == b && WIFEXITED(st) && WEXITSTATUS(st) == 0, "console B's own checks passed");
    ok = 0;
    for (int i = 0; i < 300 && !ok; i++, usleep(10000)) ok = P->ldn_get_network(P->ctx, &cur, &reason) == 0 && cur.node_count == 1;
    CHECK(ok, "A: a member that quits the room leaves the network");
    P->ldn_destroy(P->ctx);
    net_room_stop();
    CHECK(!net_room_up(), "A: left the room");
    CHECK(P->net_info(P->ctx, &ni) == 0 && !ni.connected, "A: nifm: not connected while outside the room");
    kill(srv, SIGTERM);
    waitpid(srv, NULL, 0);
    printf("test_net_room: %d checks, %d failed\n", g_checked, g_failed);
    return g_failed ? 1 : 0;
}
