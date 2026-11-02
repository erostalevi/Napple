/* Napple rooms: local wireless (LDN) and LAN play between Napple instances over the internet, through a napple-room server (crates/napple-room).
 *
 * With NAPPLE_NET=host and NAPPLE_ROOM=<host>[:<port>][/<room>] the host provider joins that room. The server gives this console an address in the
 * room's virtual LAN (10.13.0.0/16 by default) and relays traffic between the members:
 *   - virtual LAN (net_room_vlan.c): nifm reports the virtual address as the console's wired connection; guest sockets bound to it (or to INADDR_ANY) and
 *     guest datagrams / connections to virtual addresses or broadcasts are carried through the room. Each such guest socket stays a real host socket
 *     (bound to an ephemeral port, so several consoles share one Mac): datagrams from the room are injected into it over loopback with a small header
 *     that names their virtual source, and TCP connections are spliced through loopback connections, so blocking, timeouts and poll work unchanged.
 *   - LDN (net_room_ldn.c): a hosted network is advertised to the server, scans ask the server, the session frames (JOIN / ACCEPT / NODES / BYE) travel
 *     as room packets; node addresses are virtual addresses, so the title's own data goes over the virtual LAN.
 * Without NAPPLE_ROOM nothing here is active and the host provider behaves as before.
 *
 * Wire format: crates/napple-room/src/proto.rs (version 1). Packet bodies are this runtime's: u8 proto (1 UDP, 2 TCP, 3 LDN), u8 kind, u16 src_port,
 * u16 dst_port (little-endian), payload. Our own protocol; nothing Nintendo-specific beyond the LDN / LAN data the games already send. */
#pragma once
#include "online_provider.h"
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

#define NR_PROTO_VERSION 1u
#define NR_DEFAULT_PORT 31415

enum { NR_T_HELLO = 1, NR_T_WELCOME, NR_T_ERROR, NR_T_PING, NR_T_PONG, NR_T_MEMBER_JOIN, NR_T_MEMBER_LEAVE, NR_T_PACKET, NR_T_LDN_ADVERTISE, NR_T_LDN_SCAN,
       NR_T_LDN_SCAN_RESULT, NR_T_LIST, NR_T_LIST_RESULT };
enum { NR_BODY_UDP = 1, NR_BODY_TCP = 2, NR_BODY_LDN = 3 };
enum { NR_TCP_SYN = 1, NR_TCP_SYNACK, NR_TCP_RST, NR_TCP_DATA, NR_TCP_FIN };
#define NR_BODY_HDR 6u
#define NR_MAX_BODY (66u * 1024u)

/* ---- the room connection (net_room.c), one per process ---- */
/* Joins the room NAPPLE_ROOM names (once; later calls return the first outcome). Waits up to 5 s for the server's welcome; when that fails the
 * connection keeps being retried in the background. 0 = configured (joined or retrying), -1 = NAPPLE_ROOM unset or unusable. */
int net_room_start(void);
/* The same with explicit settings (tests). server "host[:port]". */
int net_room_start_with(const char *server, const char *room, const char *password, const char *nick, int vlan);
void net_room_stop(void);
int net_room_configured(void); /* NAPPLE_ROOM given (or start_with called) */
int net_room_up(void);         /* joined right now */
int net_room_vlan_on(void);    /* joined and the virtual LAN is enabled (NAPPLE_VLAN not "off") */
uint32_t net_room_vip(void);   /* this console's virtual address (host order), 0 before the first welcome */
uint32_t net_room_mask(void);
/* A virtual-LAN destination: an address in the room's subnet, its broadcast, or 255.255.255.255. */
int net_room_is_vlan_dst(uint32_t ip);
int net_room_is_broadcast(uint32_t ip);
/* nifm while in a room: the virtual interface (wired, connected only while joined). 1 = answered. */
int net_room_net_info(OnlineNetInfo *out);
/* Sends one packet body to dst (host order). 0 sent / queued, -1 not joined or the queue is full (the packet is dropped, as a LAN would). */
int net_room_send_packet(uint32_t dst, uint8_t proto, uint8_t kind, uint16_t sport, uint16_t dport, const void *payload, size_t n);
/* Raw frame (type + body); used by the LDN part for advertise / scan. */
int net_room_send_frame(uint16_t type, const void *body, size_t n);
/* Is the member with this virtual address in the room? */
int net_room_member_present(uint32_t vip);
/* A nickname for the LDN node list (NAPPLE_ROOM_NICK, else the login name). */
const char *net_room_nick(void);

/* ---- inbound dispatch (called by the room worker, room lock not held) ---- */
void nr_vlan_packet_in(uint32_t src, uint32_t dst, uint8_t proto, uint8_t kind, uint16_t sport, uint16_t dport, const uint8_t *p, size_t n);
void nr_vlan_member_left(uint32_t vip);
void nr_vlan_room_down(void);
void nr_ldn_packet_in(uint32_t src, uint8_t kind, const uint8_t *p, size_t n);
void nr_ldn_scan_result(const uint8_t *body, size_t n);
void nr_ldn_member_left(uint32_t vip);
void nr_ldn_room_down(void);
/* The virtual LAN's own work in the room worker: its descriptors are polled there too. */
int nr_vlan_pollfds(struct pollfd *pf, int max);
void nr_vlan_poll_done(struct pollfd *pf, int n);

/* ---- socket hooks (net_room_vlan.c), called by net_host_sock.c. Each returns 1 when it handled the call (result in *r / *err), 0 to go on normally. */
int nr_sock_bind(int h, int fd, int type, const OnlineAddr *a, int *r, int *err);
int nr_sock_listen(int h);
int nr_sock_connect(int h, int fd, int type, const OnlineAddr *a, int *r, int *err);
int nr_sock_send(int h, int fd, int type, const void *buf, size_t n, const OnlineAddr *to, int64_t *r, int *err);
/* For a virtual-LAN datagram socket: recvfrom that strips the injection header and reports the virtual source. Same contract as recvfrom(2). */
int nr_sock_is_vlan(int h);
ssize_t nr_sock_recvfrom(int h, int fd, void *buf, size_t n, int flags, struct sockaddr *from, socklen_t *fromlen);
/* An accepted connection on a virtual-LAN listener: registers the new handle and rewrites peer (the loopback splice) to the virtual peer. */
void nr_sock_accepted(int listen_h, int h, int fd, OnlineAddr *peer);
int nr_sock_name(int h, OnlineAddr *out, int peer, int *r, int *err);
void nr_sock_closed(int h);

/* ---- LDN over the room (net_room_ldn.c): the nh_ldn_* entry points while a room is configured ---- */
int net_room_ldn_active(void);
int nr_ldn_scan(uint64_t lcid, OnlineLdnNetwork *out, uint32_t max);
uint32_t nr_ldn_create(const OnlineLdnNetwork *cfg);
void nr_ldn_destroy(void);
uint32_t nr_ldn_connect(const OnlineLdnNetwork *net);
void nr_ldn_disconnect(void);
uint32_t nr_ldn_get_network(OnlineLdnNetwork *out, uint16_t *reason);
uint32_t nr_ldn_set_advertise(const void *data, uint32_t n);
uint32_t nr_ldn_reject(uint32_t ipv4);
int nr_ldn_get_nodes(OnlineLdnNode *out, uint32_t max, uint32_t *self);
void nr_ldn_set_notify(void (*cb)(void *arg), void *arg);

/* The LDN wire encoding shared with net_host_ldn.c (its NetworkInfo / node list layout). 0 on overflow / malformed. */
size_t nh_ldn_encode_net(const OnlineLdnNetwork *n, uint8_t *buf, size_t cap);
size_t nh_ldn_decode_net(const uint8_t *buf, size_t n, OnlineLdnNetwork *out);
size_t nh_ldn_encode_nodes(const OnlineLdnNode *nodes, uint8_t *buf, size_t cap);
size_t nh_ldn_decode_nodes(const uint8_t *buf, size_t n, OnlineLdnNode *nodes);
