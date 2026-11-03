# Network providers (OnlineProvider)

Every network-facing HLE service of the runtime goes through one extension point, `OnlineProvider` (`runtime/src/services/online_provider.h`). The services decode the
guest's request, call the active provider and encode its answer; they never open a socket, resolve a name or contact a server themselves.

The repository ships exactly one provider, the **NullProvider**: a console with no network. A private build, another user or an external module supplies a different one.
**This repository contains no online service implementation of any kind: no endpoints, tokens, certificates or protocol code, and none will be accepted.**

The one exception is Napple's own community service for Napple players, never Nintendo's: [napple-online.md](napple-online.md) (a Napple-run server that issues its own identities; it never contacts Nintendo servers and ships no console keys, certificates or Nintendo-issued tokens).

## What goes through the provider

| Service | Provider entry points | NullProvider answer |
|---|---|---|
| `bsd:u`/`bsd:s`/`bsd:a` | `sock_open`, `sock_close`, `sock_bind`, `sock_listen`, `sock_accept`, `sock_connect`, `sock_send`, `sock_recv`, `sock_shutdown`, `sock_getsockname`, `sock_getpeername`, `sock_setsockopt`, `sock_getsockopt`, `sock_set_nonblock`, `sock_poll` (also serves Select) | sockets can be created, bound and closed; `connect` fails `ENETDOWN`, `recv` `ENOTCONN`/`EAGAIN`, `send` `ENOTCONN`/`EDESTADDRREQ`, unknown descriptors `EBADF` (`offline_core.c`) |
| `nifm:u`/`nifm:a`/`nifm:s` | `net_info`, `net_connect`, `net_release` | requests are rejected at `Submit` with `HZ_R_NIFM_NETWORK_COMMUNICATION_DISABLED`, status disconnected, IP 0 |
| `sfdnsres` | `dns_resolve`, `dns_reverse` | `EAI_AGAIN` (getaddrinfo) / `TRY_AGAIN` (hostent): "no reachable resolver" |
| `ssl` | `tls_new`, `tls_set_host`, `tls_handshake`, `tls_read`, `tls_write`, `tls_pending`, `tls_free` | no handshake can run: `HZ_R_SSL_NOT_CONNECTED` |
| `friend:u` (+v/m/s/a) | `friend_count`, `friend_list`, `presence_update` | no friends; presence updates dropped |
| `nsd:u`/`nsd:a` | `nsd_resolve` | names resolve to themselves, nothing is contacted |
| `prepo:*` | `report` | play reports dropped |

Not routed (not network-facing in this runtime, or owned elsewhere): `bcat` network parts, `ldn`, `account` network service calls, `olsc`.

## The ABI (version 1)

One C struct of function pointers, `OnlineProvider`, with `abi_version` (must be `ONLINE_ABI_VERSION`), `struct_size` (the size the module was built with; fields past it are
treated as absent, so a module built against an older header still works), `name`, `caps`, `ctx` (the first argument of every call), optional `init`/`shutdown` and the entry
points above. `caps` (`ONLINE_CAP_NETWORK`, `_SOCKETS`, `_DNS`, `_TLS`, `_FRIENDS`, `_REPORTS`, `_NSD`) says what the provider really does; a missing entry point answers like the
NullProvider (`ONLINE_FN(p, field)` returns `NULL` for it).

Conventions:
- Socket calls return `>= 0` or `-1` with `*err` set to a FreeBSD errno (`hz_result.h`: `HZ_ECONNREFUSED` and so on), exactly what the guest sees. Handles are the provider's own small
  integers; the runtime maps guest descriptors (lowest free first, at most 128) to them (`bsd_core.c`).
- Addresses are `OnlineAddr` in host byte order (`family` 2 or 28, `port`, 16 address bytes); the runtime converts from and to the guest `sockaddr`.
- `dns_resolve` returns 0 with `OnlineResolved` filled, or an `HZ_EAI_*` number; the hostent calls map it to a netdb error. The runtime serialises the answer in the resolver
  client library's format (`sfdnsres_core.c`).
- `tls_handshake` returns 0 or non-zero with `*result` set to the Horizon result to report.
- `net_connect` returns 0 (accepted) or a Horizon result code that `nifm` `GetResult` reports.
- Calls may block (`sock_recv`, `sock_poll`, `tls_*`); the runtime does not hold its descriptor-table lock across them. Calls arrive from guest threads, concurrently.

## Loading a provider

1. `NAPPLE_ONLINE_PROVIDER=/path/to/provider.dylib` (read once, at the first network call), or the `online_provider` key of `<support>/system.json`, which Studio's Settings > Online edits and the runtime turns into that variable at boot (the environment wins). Empty = the NullProvider (offline).
2. The library exports `const OnlineProvider *napple_online_provider_v1(void)` (`ONLINE_PLUGIN_SYMBOL`).
3. The runtime checks `abi_version` and `struct_size`, calls `init` and uses the table. Any failure (a path that does not exist, a wrong ABI, a missing symbol) is logged as one `online:` line and the NullProvider stays in force; the game keeps running offline.
4. Embedding hosts and tests can call `online_provider_install(&table)` instead; `NULL` restores the NullProvider.

iOS builds never load provider modules (no runtime code loading); the iOS app can only link a provider at build time.

## Writing one (checklist)
- Fill every entry point you support and set the matching `caps` bit; leave the others `NULL`.
- Never block forever without honouring `sock_poll` timeouts; return `HZ_EAGAIN` for non-blocking sockets.
- Do not log guest data. Keep tokens, endpoints and certificates in your own module; nothing of it belongs in this repository.
- Test it the way `runtime/tests/test_svc_online.c` does: `online_loopback.h` is a complete in-memory provider (sockets, DNS, TLS pass-through, friends) that serves a bsd echo
  connection. Run `sh tools/linux-check/run-tests.sh runtime/tests/test_svc_online.c`.

### Additions after version 1 (struct_size-extensible)
`ABI_VERSION` stays 1: the table grew at the end, and a module built with the older, smaller `struct_size` simply lacks the new entry points (`ONLINE_FN` returns NULL, the service
answers like the NullProvider). New capability bits: `ONLINE_CAP_LDN` (1<<7) and `ONLINE_CAP_ACCOUNT` (1<<8).
- **ldn** (local wireless): `ldn_scan(lcid, out, max)`, `ldn_create(cfg)`, `ldn_destroy`, `ldn_connect(net)`, `ldn_disconnect`, `ldn_get_network(out, &disconnect_reason)`,
  `ldn_set_advertise`, `ldn_reject(ipv4)`. The service keeps the state machine and encodes `OnlineLdnNetwork` into the guest's 0x480-byte NetworkInfo; the provider owns the transport.
- **acc**: `account_info(user_id, out)` fills `OnlineAccountInfo` {linked, id, 0x68 opaque resource-cache bytes}; the service hands them to `GetAccountId` and
  `GetNintendoAccountUserResourceCacheForApplication`. Without a linked account the answers are the offline ones (fixed local id, zero cache), never an error.
- **bsd** `DuplicateSocket`, `SendMMsg`, `RecvMMsg` need no new entry point: they use the existing socket calls.

## Status
The provider ABI and the services over it are tested against the mock IPC layer and the loopback provider (`test_svc_online`); the built-in host provider below is tested on macOS by
`make -C runtime test-net-host`. Argument layouts of the bsd/sfdnsres/ssl/friend commands follow the public documentation; a row is *verified* only once a title has used it.
History of the interface: the [change archive](../archive/changes/README.md); the provider-backed services were introduced in the [sim-online note](../archive/changes/2026-10-26-sim-online-2026-10-26-macos1.md).

## The built-in host provider (`NAPPLE_NET=host`)

An opt-in provider that ships with the runtime (`runtime/src/net/`): the title's network traffic goes through the Mac's own network stack. The default stays
offline; `NAPPLE_NET=host` selects it (`NAPPLE_ONLINE_PROVIDER`, when set, wins, even if its module fails to load). It is **generic transport only**: it
contains no endpoint, token, certificate or protocol of any online service (the only names in it are the vendor domains it refuses, below) and implements no account or device authentication. Friends,
presence, play reports, nsd, network accounts and every other online-service call keep their offline answers (their entry points are `NULL`), so a title that
needs a signed-in account still sees "no account" and ends its online attempt the way it does offline.

| Capability | Built on | Behaviour |
|---|---|---|
| `ONLINE_CAP_SOCKETS` (bsd) | BSD sockets (`net_host_sock.c`) | Real TCP / UDP (raw sockets need root and fail like the host does). Host descriptors are always non-blocking; the guest's blocking mode (Fcntl / FIONBIO / `SOCK_NONBLOCK` / `MSG_DONTWAIT`), `SO_RCVTIMEO` / `SO_SNDTIMEO` and `MSG_WAITALL` are applied by waiting in 50 ms poll slices, so a Close or ShutdownAllSockets from another guest thread ends a wait (EBADF). Options and flags are FreeBSD's (Darwin shares them; `TCP_KEEPIDLE/INTVL/CNT` are mapped); options the Mac lacks are accepted and ignored (logged once). Errors are FreeBSD errno numbers. |
| `ONLINE_CAP_DNS` (sfdnsres) | `getaddrinfo` / `getnameinfo` (`net_host_dns.c`) | The Mac's resolver, `/etc/hosts` and DNS settings. Names are not rewritten (nsd keeps answering names unchanged). |
| `ONLINE_CAP_TLS` (ssl) | Security.framework SecureTransport over the guest's socket (`net_host_tls.m`) | TLS 1.2+ client. The server is verified against the **host trust store** plus the CA certificates the guest imported for the connection (`tls_add_ca`, DER or PEM); the guest's verify option (`tls_set_verify`: PeerCa, HostName) can relax it. SNI from SetHostName. |
| `ONLINE_CAP_NETWORK` (nifm) | SystemConfiguration dynamic store + `getifaddrs` (`net_host_nifm.m`) | Connected while the Mac has a primary IPv4 interface with an address; reports it as a wired connection (no SSID) with the real IP, mask, router and DNS servers. Requests are accepted; without a connection they are refused like offline. |
| `ONLINE_CAP_LDN` (ldn) | UDP discovery + TCP session between Napple instances (`net_host_ldn.c`) | Local wireless over the LAN. A node's address in the network is its real LAN address (the ldn_mitm idea), so the title's own sockets reach the other consoles. Discovery: UDP `NAPPLE_LDN_PORT` (default 11452) .. +7 on 127.0.0.1, the broadcast address and `NAPPLE_LDN_PEERS`; scans wait `NAPPLE_LDN_SCAN_MS` (150). The host tells stations about joins, leaves, new advertise data, Reject and DestroyNetwork. |

### The console vendor's servers are never reached
In this provider's DNS path (sfdnsres, the only place it resolves names) every name under `nintendo.net`, `nintendo.com`, `nintendo.co.jp`, `nintendo.eu`,
`nintendowifi.net`, `nintendo-europe.com` and `nintendoswitch.com` (the domain itself and any subdomain, any case, trailing dot allowed) answers
**host not found** (`EAI_NONAME`), exactly what an offline resolver gives, so a title ends its online attempt as it would with a dead DNS. The one way
around it is the redirect hook `net_host_redirect_lookup` (`runtime/src/net/net_host.h`): `int (*)(const char *name, char *out, size_t n)`, NULL by default,
called for every name before the block check. It returns 1 with the name or address of a configured Napple server in `out`, or 0. A redirect whose target
is itself a blocked name is refused. Nothing in the runtime sets it by default. A title that connects to a literal IP address bypasses DNS: not covered.

### ABI additions (struct_size-extensible, `ABI_VERSION` still 1)
- `tls_add_ca(conn, cert, n)`: trust a guest-imported CA for this connection; `tls_set_verify(conn, option)` with `ONLINE_TLS_VERIFY_PEER_CA` / `_HOST_NAME` / `_DATE`.
- `ldn_get_nodes(out, max, &self)` with `OnlineLdnNode {ipv4, mac, node_id, connected, user_name}`: every node of the network (the service writes all of them into
  NetworkInfo instead of the host alone); `ldn_set_notify(cb, arg)`: the provider calls `cb` on any change (the service fires the state change event, and a station
  whose host left goes back to StationOpened with the reason).
- `online_builtin_host` / `ONLINE_ENV_NET`: how a linked module registers itself for `NAPPLE_NET=host` (the runtime's `src/net/` does so from a constructor).

### Limits
- Two instances on one Mac share the Mac's address: a title that binds one fixed port on every node can only run one node per Mac (use two Macs).
- The station's user name, the passphrase, the accept policy and MAC filters are not passed to the provider (the service keeps the last three); stations show an
  empty user name. ldn GetIpv4Address still reports the mask 255.255.0.0.
- The ssl service hands the context's ImportServerPki certificates (PEM or DER) and an explicit SetVerifyOption to the provider at DoHandshake; ImportClientPki,
  client certificates and the console's built-in CA list (GetCertificates) are not used: the host trust store stands in for the latter.
- TLS 1.0/1.1-only servers are refused; no client certificates.

Tests: `make -C runtime test-net-host` (also part of `test-services`): localhost TCP/UDP, non-blocking connect + poll, timeouts, DNS, nifm, TLS against a
throw-away local CA served by `/usr/bin/openssl s_server`, LDN between provider instances, and the same through the bsd, ssl and ldn handlers.
