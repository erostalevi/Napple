# napple-online (2026-11-02-r7): a Pretendo-style Napple online service (MK8D first)

Branch `claude/napple-online-2026-11-02-r7`, Mac session. Integrator file: `integrators/claude-napple-online-2026-11-02-r7.md`. Guide:
`docs/guides/napple-online.md`.

## What changed
- `crates/napple-online/` (new, workspace member): the server. Napple accounts and HS256 tokens, an HTTP account API and a WebSocket game server on
  one port, PRUDP Lite (handshake, reliable ordered delivery, single and aggregate acks, fragments, retransmission, ping, disconnect), RMC, NEX
  types, Kerberos tickets with server-chosen source keys, and the protocols Authentication, SecureConnection, MatchmakeExtension, MatchMaking,
  MatchMakingExt, NATTraversal, Health, Utility, Ranking and Notification (server to client). Binary `napple-online serve`.
- `runtime/src/net/net_online{.h,.c,_http.c}` (new): with `NAPPLE_NET=host` and `NAPPLE_ONLINE_SERVER=http(s)://host[:port]`, the host provider
  is wrapped. Game-server names resolve to the virtual address 100.78.78.100, which connects to the Napple server. Other Nintendo names are
  blocked. The title's TLS to the server ends in the glue, with IoMode honoured. acc gets a Napple account (created on first use, credentials
  under the support dir) and a Napple ID token. `NAPPLE_ONLINE_DEBUG=1` logs every step. Without `NAPPLE_ONLINE_SERVER`, `NAPPLE_NET=host` is the
  bare host provider (r6).
- `runtime/src/services/online_provider.h` (append): `account_id_token`, `tls_set_io_mode`, `ONLINE_TLS_IO_*`; the scope note now describes the
  Napple-only service.
- `runtime/src/services/account.c`: EnsureIdTokenCacheAsync (IAsyncContext), LoadIdTokenCache(Deprecated) answer from the provider when it has
  accounts; otherwise unchanged.
- `runtime/tests/test_net_online_client.c` + Makefile rule `test-net-online` (part of `test-services`).

## Verified on MK8D (update v1376256)
With a local server and the glue (plus net-host-provider's bsd Poll timeout fix, merged), the
title does the following:
- gets a Napple account and token;
- resolves its game server to the Napple server;
- logs in over WebSocket + PRUDP Lite (Authentication, ticket, secure CONNECT, Register, Utility, Ranking common data);
- shows the online menu;
- after Worldwide and character selection, calls AutoMatchmakeWithParam_Postpone.

It then shows "A communication error has occurred.", so the race lobby is not reached yet.

## Also
- Server deployment contract: `NAPPLE_ONLINE_HTTP` / `_DATA` / `_VERBOSE` / `_PUBLIC_HOST`, `GET /healthz`, logs on stdout, X-Forwarded-For.
  NEX needs no UDP ports (it runs in the WebSocket). Client: `NAPPLE_ONLINE_SERVER=default` is `https://pretend.napple.lat` (opt-in).
- Gate: `cargo test -p napple-online` 31/0; `make -C runtime test test-services test-ipc-reference` exit 0 (test_net_online_client 45/0).
- Four titles boot 30 s with and without `NAPPLE_NET=host`, no regression.
- ssl (orchestrator decision, net-host-provider frozen): the ssl functions of `offline.c` hand IoMode to the provider and answer would-block with
  ResultWouldBlock (test `test_net_online_ssl`). MK8D's login is verified from the branch alone.
- Needs from other areas (sfdnsres and nifm byte order, prepo's second buffer): see the integrator file.
