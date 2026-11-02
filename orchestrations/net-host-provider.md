---
name: net-host-provider
batch: 2026-11-02-r6
branch: claude/net-host-provider-2026-11-02-r6
base: orchestrate/2026-11-02-r6
env: mac
model: claude-opus-5-5
depends_on: []
status: launched
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_3352261a-bf2a-4f07-a48b-c8b0261a4645 (fresh)
launched_at: 2026-11-02
---

# net-host-provider: a built-in, opt-in macOS host network provider (sockets, DNS, TLS, nifm connectivity, LDN over LAN)

## Goal
The user wants 100% coverage; the largest remaining block (~0.4 pts) is the network services, which answer like a console with no network. The repository has a seam for this (`runtime/src/services/online_provider.h`, NullProvider by default, external providers via `NAPPLE_ONLINE_PROVIDER`). Add a built-in **host provider** selected with `NAPPLE_NET=host` (default stays offline) implementing the generic, non-Nintendo capabilities:
- `ONLINE_CAP_SOCKETS` (bsd:u/bsd:s): real BSD sockets on macOS (socket/bind/connect/listen/accept/send/recv/sendto/recvfrom/poll/select/fcntl/setsockopt/getsockopt/shutdown/close/getaddrinfo-style helpers), FreeBSD error numbering per the header, non-blocking + poll semantics, guest sockaddr conversion.
- `ONLINE_CAP_DNS` (sfdnsres): getaddrinfo/gethostbyname/gethostbyaddr via the host resolver.
- `ONLINE_CAP_TLS` (ssl): TLS client contexts and connections over the host stack (Network.framework or Security), using the HOST trust store for server verification; import of guest-supplied CA certs for that context.
- `ONLINE_CAP_NETWORK` (nifm): connected / IP / SSID-less wired-style info derived from the host's real connectivity (SCNetworkReachability or nw_path_monitor).
- `ONLINE_CAP_LDN` (ldn:u): local-wireless sessions emulated over the LAN (UDP broadcast discovery + TCP/UDP session transport between Napple instances), scan/create/connect/disconnect/state events.
NOT in scope (stays offline, must not be implemented): Nintendo account/device authentication (dauth/aauth/baas), friend/npns/prepo/bcat/eShop servers, any Nintendo endpoint, token, certificate or protocol. The provider must contain no Nintendo hostnames or credentials.

## Owned paths
New files `runtime/src/net/` (host provider: `net_host.c`/`.h`, `net_host_sock.c`, `net_host_dns.c`, `net_host_tls.m`, `net_host_nifm.m`, `net_host_ldn.c`), provider selection in `runtime/src/services/online_core.c` (minimal), `runtime/src/services/bsd_core.[ch]`, `sfdnsres*.c`, `ldn*.c` where the service side needs to forward more calls to the provider, new tests `runtime/tests/test_net_host_*.c`, `docs/guides/network-providers.md` (append a host-provider section), `docs/reference/environment-variables.md` (append `NAPPLE_NET`), own change note. `runtime/Makefile`: append-only (new sources, frameworks Network/Security/SystemConfiguration).

## Do not touch
`runtime/src/services/offline.c` and `offline_core.c` (nifm-netconnect of round 5 is editing them; if a service-side change there is needed, write it under "Needs from other areas" or make it after merging main once nifm-netconnect has landed — check `git log main --oneline | grep nifm-netconnect`), `applet*.c`, `gfx/`, `crates/`, `runtime/src/a32*`, `kernel.c`, `orchestrations/`.

## Acceptance
1. With `NAPPLE_NET=host`: unit/integration tests against localhost — TCP echo, UDP, non-blocking connect + poll, DNS of `localhost` and of a public name (skip with a note if offline), TLS handshake to a local test server with a self-signed cert added as a guest CA, nifm reports connected with the host's IP; with the variable unset every service answers exactly as before (existing offline tests unchanged and green).
2. LDN over LAN: two runtime instances (or two provider instances in one test) on this Mac discover, create, join and exchange a packet.
3. Real titles: four-title 60 s boots with `NAPPLE_NET=host` — no crash, no new missing IPC; report what each title does with the network (MK8D Online Play / SMO / ACNH network checks): it may now reach Nintendo's servers and fail authentication — that is expected and must end cleanly (no hang, no crash).
4. Four-title boots without the variable: unchanged.
5. `make -C runtime test test-services test-ipc-reference` green.

## Starting points
`runtime/src/services/online_provider.h` (the ABI), `docs/guides/network-providers.md`, `online_core.c`, `bsd_core.c`, `sfdnsres_core.c`, `ldn_core.c`, `offline_core.c` (read only).

## Env notes
Read `orchestrations/ENV-MAC.md` (shared title data read-only, locks, local-only, never ask). Never commit credentials or Nintendo hostnames.

## Result
