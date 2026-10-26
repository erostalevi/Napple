# Network providers (OnlineProvider)

Every network-facing HLE service of the runtime goes through one extension point, `OnlineProvider` (`runtime/src/services/online_provider.h`). The services decode the
guest's request, call the active provider and encode its answer; they never open a socket, resolve a name or contact a server themselves.

The repository ships exactly one provider, the **NullProvider**: a console with no network. A private build, another user or an external module supplies a different one.
**This repository contains no online service implementation of any kind: no endpoints, tokens, certificates or protocol code, and none will be accepted.**

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

1. `NAPPLE_ONLINE_PROVIDER=/path/to/provider.dylib` (read once, at the first network call).
2. The library exports `const OnlineProvider *napple_online_provider_v1(void)` (`ONLINE_PLUGIN_SYMBOL`).
3. The runtime checks `abi_version` and `struct_size`, calls `init` and uses the table. Any failure is logged (`online:` line) and the NullProvider stays in force.
4. Embedding hosts and tests can call `online_provider_install(&table)` instead; `NULL` restores the NullProvider.

iOS builds never load provider modules (no runtime code loading); the iOS app can only link a provider at build time.

## Writing one (checklist)
- Fill every entry point you support and set the matching `caps` bit; leave the others `NULL`.
- Never block forever without honouring `sock_poll` timeouts; return `HZ_EAGAIN` for non-blocking sockets.
- Do not log guest data. Keep tokens, endpoints and certificates in your own module; nothing of it belongs in this repository.
- Test it the way `runtime/tests/test_svc_online.c` does: `online_loopback.h` is a complete in-memory provider (sockets, DNS, TLS pass-through, friends) that serves a bsd echo
  connection. Run `sh tools/linux-check/run-tests.sh runtime/tests/test_svc_online.c`.

## Status
Tested on Linux against the mock IPC layer and the loopback provider; not yet run on macOS or against a title. Argument layouts of the bsd/sfdnsres/ssl/friend commands are from
memory of the public documentation and unverified. See the [hand-off notes](../archive/changes/2026-10-26-claude-sim-online-2026-10-26-macos1.md).
