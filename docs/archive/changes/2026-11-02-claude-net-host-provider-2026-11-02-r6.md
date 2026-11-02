# net-host-provider (2026-11-02-r6): a built-in, opt-in host network provider

Branch `claude/net-host-provider-2026-11-02-r6`, Mac session. Integrator file: `integrators/claude-net-host-provider-2026-11-02-r6.md`.

## What changed
- `runtime/src/net/` (new): the host provider behind `OnlineProvider`, selected with `NAPPLE_NET=host` (default offline, `NAPPLE_ONLINE_PROVIDER` wins).
  - `net_host_sock.c`: real BSD sockets. Host descriptors always non-blocking; guest blocking mode, `SO_RCVTIMEO`/`SO_SNDTIMEO`, `MSG_DONTWAIT`, `MSG_WAITALL`
    applied by waiting in 50 ms poll slices (a Close from another guest thread ends a wait with EBADF); waits give the virtual core away (`thread_core_away`).
    FreeBSD option / flag / errno numbering (Darwin's above 65 mapped).
  - `net_host_dns.c`: `getaddrinfo` / `getnameinfo`, EAI mapping.
  - `net_host_tls.m`: SecureTransport client over the guest's socket; verification against the host trust store plus guest-imported CAs; verify option.
  - `net_host_nifm.m`: SystemConfiguration (primary IPv4 interface, router, DNS) + `getifaddrs`; wired-style connection; 1 s cache.
  - `net_host_ldn.c`: LDN over the LAN (UDP discovery on `NAPPLE_LDN_PORT`..+7 via 127.0.0.1, broadcast, `NAPPLE_LDN_PEERS`; TCP session JOIN / ACCEPT /
    REJECT / NODES / BYE; node addresses are real LAN addresses).
  - `net_host.c`: table, instances (tests), errno / address helpers, registration with `online_core.c` from a constructor.
- `online_provider.h` (append): `tls_add_ca`, `tls_set_verify`, `ldn_get_nodes` (+ `OnlineLdnNode`), `ldn_set_notify`, `ONLINE_TLS_VERIFY_*`, `ONLINE_ENV_NET`,
  `online_builtin_host`. `ABI_VERSION` unchanged (struct_size-extensible).
- `online_core.c`: `NAPPLE_NET` selection (`host`, `offline`; anything else logged, offline).
- `ldn.c` / `ldn_core.[ch]`: NetworkInfo carries every provider node (`ldn_net_encode_nodes`), NodeInfo carries the local communication version (encode and
  decode), the state change event fires on provider notifications, a station whose host left returns to StationOpened, Finalize / closing the object end
  the network and the notification registration.
- Tests `runtime/tests/test_net_host_{sock,tls,ldn}.c` (`make -C runtime test-net-host`, part of `test-services`). Makefile: `src/net` sources, Security and
  SystemConfiguration frameworks, the test rules (additive lines only).
- Docs: host-provider section in `docs/guides/network-providers.md`, `NAPPLE_NET` / `NAPPLE_LDN_*` in `docs/reference/environment-variables.md`.

Nothing in the provider knows an online service: no hostnames, endpoints, tokens, certificates, account or device authentication. Friends, presence, play
reports, nsd and network accounts keep their offline answers (NULL entry points).

## Status
- Verified here (Mac, macOS stack): sockets, DNS (localhost and a public name), nifm (the Mac's address), TLS (throw-away local CA, public server against
  the host trust store), LDN between provider instances and through `ldn:u`, the bsd / ssl / ldn / nifm handlers routing to it. `make -C runtime test
  test-services test-ipc-reference` green.
- Title boots: see the integrator file (four titles 60 s with and without `NAPPLE_NET=host`, MK8D Online Play).
- Not done here: the ssl service does not hand ImportServerPki certificates or SetVerifyOption to the provider (offline.c belongs to nifm-netconnect this round).
