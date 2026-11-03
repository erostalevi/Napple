---
branch: claude/napple-online-2026-11-02-r7
orchestration: napple-online
base: c44af84
head: f33ae29
env: mac
date: 2026-11-02
areas: [crates/napple-online, runtime/net, runtime/services (account.c, online_provider.h)]
status: integrated
---

# claude/napple-online-2026-11-02-r7: a Pretendo-style Napple online service (MK8D first)

## Summary
A community-run online service for Napple instances, never Nintendo:
- **Server, `crates/napple-online`** (binary `napple-online`, std + serde + sha2 only). It provides:
  - Napple accounts and HS256 tokens over a small HTTP API;
  - a NEX game server inside WebSocket on the same port: PRUDP Lite, RMC, and Kerberos tickets sealed with server-chosen keys;
  - the protocols Authentication, SecureConnection, MatchmakeExtension, MatchMaking, MatchMakingExt, NATTraversal (probe forwarding), Health,
    Utility and Ranking (common data, empty rankings), plus Notification events to clients;
  - env-var configuration, `GET /healthz`, logs on stdout.
- **Client glue, `runtime/src/net/net_online*`**: a wrapper over the host provider, on with `NAPPLE_NET=host NAPPLE_ONLINE_SERVER=…`
  (`default` = `https://pretend.napple.lat`). It does the following:
  - acc gets a Napple account (created on first use, saved per server and user) and a Napple ID token (EnsureIdTokenCacheAsync / LoadIdTokenCache);
  - nsd fills `%` with `lp1`;
  - a NEX game-server name resolves to the virtual address 100.78.78.100, which connects to the Napple server; every other Nintendo name gets EAI_NONAME;
  - the title's TLS to the server ends in the glue (plain for http://, real TLS to the server's own name for https://) and honours IoMode.

**Verified on MK8D** (update v1376256, local server) from this branch's code alone (the ssl change MK8D needs is in the branch; see Files):
- the account and token are issued;
- the game server resolves to the Napple server;
- WebSocket + PRUDP Lite login works: Authentication with the Napple token, the ticket, secure CONNECT, Register, Utility, Ranking;
- **the online menu (Worldwide / Regional / Friends / Tournaments) is shown**;
- after Worldwide and character/kart selection, AutoMatchmakeWithParam_Postpone creates a gathering.

**Not done:** the title then shows "A communication error has occurred." (play report: error 2618-0562 `SessionKeepFailed`, a Pia error, raised
before Pia opens any socket). So no race lobby, and two instances were not matched in a real run; that is only covered by unit tests. DataStore,
Competition and SimpleSearchObject methods are not implemented.

## Files
- New: `crates/napple-online/` (Cargo.toml, src/{lib,main,accounts,crypto,http,kerberos,matchmaking,nex,prudp,rmc,server,session}.rs).
- New: `runtime/src/net/net_online.h`, `net_online.c`, `net_online_http.c`; `runtime/tests/test_net_online_client.c`.
- New: `docs/guides/napple-online.md`, `docs/archive/changes/2026-11-02-napple-online-2026-11-02-r7.md`, this file.
- Changed by orchestrator decision (net-host-provider is frozen): the **ssl functions** of `runtime/src/services/offline.c` only
  (`ssl_set_io_mode`, `ssl_handshake`, `ssl_read`, `ssl_write`). They hand IoMode to the provider's `tls_set_io_mode` and answer a would-block read
  or write with `HZ_R_SSL_WOULD_BLOCK` (2123-0204, new in `hz_result.h`). Test `runtime/tests/test_net_online_ssl.c` (7dde717). Nothing in the
  nifm rows (mk8d-lan-session's).
- Changed (owned): `runtime/src/services/account.c` (IManagerForApplication 2/3/4 from the provider's `account_id_token`, IAsyncContext; the old
  generic error when the provider has no accounts).
- Shared, append-only: `runtime/src/services/online_provider.h`:
  - `account_id_token` and `tls_set_io_mode` appended after net-host-provider's fields;
  - `ONLINE_TLS_IO_*`;
  - the scope note rewritten as the orchestration asked.
- Shared, append-only: `Cargo.toml` (member), `Cargo.lock` (+ the package), `runtime/Makefile` (a `test-net-online` block at the end, part of
  `test-services`).
- Two merges of `claude/net-host-provider-2026-11-02-r6` (final 80a8880), nothing of theirs edited.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Acceptance 1: trace of MK8D's online path | verified | table in `docs/guides/napple-online.md` (runs with `NAPPLE_TRACE_IPC=all NAPPLE_ONLINE_DEBUG=1`, server `-v`) |
| Acceptance 2: `cargo test -p napple-online` covering the items below | tested | 31 passed, 0 failed |
| — token issue / verify, account store persistence | tested | (part of the 31) |
| — PRUDP handshake / ack / aggregate ack / fragments / retransmit with backoff / ping / disconnect | tested | (part of the 31) |
| — RMC encode / decode | tested | (part of the 31) |
| — Authentication login (methods 2 and 6) + ticket + RequestTicket | tested | (part of the 31) |
| — Secure CONNECT (forged ticket refused) and Register | tested | (part of the 31) |
| — full login + two clients matched into one room over PRUDP in process | tested | (part of the 31) |
| Acceptance 3: with `NAPPLE_ONLINE_SERVER`, MK8D gets Napple tokens and its game-server name goes to the Napple server | verified | run log: `online: napple: local user linked to Napple account …`, `ID token issued (279 bytes) for title 0100152000022000`, `redirected name g2b309e01-lp1.s.n.srv.nintendo.net`, `port 443 -> 127.0.0.1:8737 (http)` (copy in the shared notes, `napple-online-r7/runtime-log-mk8d-login.txt`) |
| Acceptance 4: MK8D connects, completes login, reaches the online menu | verified (branch head, nothing applied locally) | server log `account … connected to the secure server` (verbose runs: `Authentication (10) method 2 -> ok`, `SecureConnection (11) method 1 -> ok`); screenshot `napple-online-r7/mk8d-online-menu-branch-head.png` (frame 1800) |
| ssl non-blocking (IoMode, ResultWouldBlock) | tested + verified | `test_net_online_ssl` 42 checks, 0 failed; MK8D run above |
| Acceptance 4 stretch: two instances in one room | not done | the single-instance Worldwide path already fails with 2618-0562 (see Findings) |
| Acceptance 5: without `NAPPLE_ONLINE_SERVER` = r6 | tested + verified | `test_net_online_client` checks NAPPLE_NET=host is the bare host provider; `make -C runtime test test-services test-ipc-reference` exit 0 (ipc_reference 111 fixtures, missing 0); four titles 30 s each, without and with `NAPPLE_NET=host` (no server): SMO 1757/1755 frames, LAS 1758/1774, ACNH 853/881, MK8D 1764/1772, rc 0, no guest Break |
| Client glue unit test | tested | `make -C runtime test-net-online` → test_net_online_client 45/0, test_net_online_ssl 42/0 |
| Server env-var contract, /healthz, stdout logs | verified | `NAPPLE_ONLINE_HTTP=127.0.0.1:8738 NAPPLE_ONLINE_DATA=… target/release/napple-online serve`: `/healthz` 200, account created, only `accounts.json` + `server.key` in the data dir |
| Server builds on Linux | proposed | no Linux target on this Mac; the crate uses only std, serde/serde_json, sha2 and one `#[cfg(unix)]` permission call |

## Unbuilt / untested list (the integrator checks these first)
- The https:// client path (real TLS to a community server through the host provider's SecureTransport, non-blocking emulation via `tls_pending`
  + poll): compiled, never run against a real TLS server (d725fb7 / b145cde, `net_online.c` `readable`, `net_online_http.c`).
- Server behind Caddy (X-Forwarded-For, WebSocket upgrade through the proxy): not run (713414e).
- Server on Linux: never compiled there.
- Matchmaking paths other than 40, 21.2: unit-tested only (no title reached them).

## Findings
- **Switch NEX is PRUDP Lite inside a WebSocket over TLS** (`wss://g<id>-lp1.s.n.srv.nintendo.net:443/`), not PRUDP V1 over UDP. Lite packets are
  unsigned and unencrypted, and with `pSourceKey` the client skips key derivation. So **the server needs no per-game secret** (no access key).
  Verified: MK8D accepts our ticket (hex source key → raw key bytes) and our CONNECT-ACK.
- Decision: the NEX ports in the infra contract (`NAPPLE_ONLINE_NEX_AUTH/SECURE`, UDP 60000/60001) and `NAPPLE_ONLINE_PUBLIC_IP` are not needed.
  Everything runs on `NAPPLE_ONLINE_HTTP`, and Caddy's default WebSocket proxying covers it. The variables are accepted and logged as unused, so
  infra can drop the UDP ports. The secure station URL is `0.0.0.1:1`, `sid=2`: same server, PRUDP port 2.
- Decision: redirect in my own wrapper (`no_dns_resolve`) rather than through `net_host_redirect_lookup`. The wrapper also has to own connect,
  TLS and acc, and a name-to-name redirect cannot express "the title's port 443 → the server's port". The hook stays unused (NULL).
  Registration order is safe: net_host.c registers only when the pointer is empty.
- Decision: the virtual address is a palindrome (100.78.78.100) because of the sfdnsres byte-order bug below.
- **Bug (sfdnsres, not mine): GetHostByNameRequest hostents come out byte-swapped.** `sfdns_serialize_hostent` copies the network-order bytes;
  MK8D connected to 79.78.127.100 for 100.127.78.79.
- **Bug (nifm, not mine): GetCurrentIpAddress is byte-swapped.** MK8D's station URL says `address=229.5.168.192` for 192.168.5.229. `nifm_ip`
  writes the host-order u32. The server overrides the public address with the observed one, so login is unaffected; P2P on a LAN may be.
- nifm `GetCurrentNetworkProfile` is a stub error. A local experiment returning a profile did not change the matchmaking error.
- The open blocker, `SessionKeepFailed` 2618-0562, happens right after AutoMatchmakeWithParam_Postpone answers.
  - Our MatchmakeSession echo is byte-consistent with the request (versions, fields, +32-byte session key, participation 1, start time).
  - Nothing else is called before the error: no Pia socket, no `nncs*` NAT-check lookup, no NATTraversal / GetRelaySignatureKey.
  - Next suspects: something Pia derives from the session or from Register's answer (the public station URL fields, `type`/`natm`/`natf`), or a
    notification Pia waits for. A capture of a real exchange is not available; compare with Pretendo's Switch work when it exists.
  - Tool: `NAPPLE_ONLINE_DEBUG=1` dumps play reports; the error report is in the prepo *second* buffer, which `prepo_deliver` does not hand to
    providers (a local debug line was used; not committed).
- The error applet path (error applet 0xe → PopOutData 0x680 → SetTerminateResult → guest Break) aborts MK8D when no token is available
  (applets' area; seen when the server is unreachable).
- `tests/services.mk`'s `svc_included` is what keeps included services out of the link; the `test_net_online_*` rule uses it like
  `test_net_host_*`.

## How to integrate
- Lands after net-host-provider (already merged here at 80a8880). Merge order with net-rooms: independent.
- Conflict hotspots:
  - `online_provider.h`: keep every appended field in order; net-host-provider's, then mine (`account_id_token`, `tls_set_io_mode`), then later ones.
  - `runtime/Makefile`, `Cargo.toml` members, `Cargo.lock`: additive.
  - `docs/guides/network-providers.md` still says "this repository contains no online service implementation … none will be accepted". It is not
    mine; append a line pointing to `napple-online.md` (the Napple-only service) when landing.
- Build:
  - `cargo test -p napple-online` → 31 passed;
  - `cargo build --release -p napple-online` → `target/release/napple-online`;
  - `make -C runtime` → no new warnings;
  - `make -C runtime test-net-online` → 45/0 and 42/0;
  - `make -C runtime test test-services test-ipc-reference` → exit 0.
- No Linux-only calls. The server crate is portable on purpose (it is meant to run in a Linux container).

## How to test
1. Unit tests: as above.
2. Local server + MK8D. Run `make -C runtime` first: the test targets rebuild objects but do not relink `napple-runtime`.
   ```bash
   target/release/napple-online serve -v
   NAPPLE_NET=host NAPPLE_ONLINE_SERVER=http://127.0.0.1:8737 NAPPLE_ONLINE_DEBUG=1 \
     NAPPLE_INPUT_SCRIPT=15:A,19:DOWN,20.5:DOWN,22:A NAPPLE_SCREENSHOT=/tmp/mk NAPPLE_SCREENSHOT_FRAMES=1800 \
     runtime/build/napple-runtime work/games/0100152000022000/fe941ed5ba14be5d --headless --seconds 40 --save-root <copy of a save past first boot>
   ```
   - Passing: the server prints `Authentication (10) method 2 -> ok`, `SecureConnection (11) method 1 -> ok`, Utility 7 ×2, Ranking 4; the frame
     1800 shows Worldwide / Regional / Friends / Tournaments.
   - A stale binary without the ssl change: one SYN, then "Connecting…" forever.
   - A fresh save stops at the first-boot "Select a Mii" screen.
3. Regression guard: without `NAPPLE_ONLINE_SERVER` the log has no `napple:` lines and `NAPPLE_NET=host` behaves as r6.
4. Human: two Macs with `NAPPLE_ONLINE_SERVER=http://<lan server>:8737` once 2618-0562 is solved.

## Fix policy hints
- Safe: error/log wording, the server's default ports, adding answers for more MatchmakeExtension / DataStore methods.
- Design decisions:
  - whether to route the glue through `net_host_redirect_lookup` instead of the wrapper's own DNS override (it cannot express the port rewrite
    today).

## Needs from other areas
1. sfdnsres: `sfdns_serialize_hostent` IPv4 byte order (MK8D reads it reversed).
2. nifm: `GetCurrentIpAddress` byte order; `GetCurrentNetworkProfile` / `GetCurrentIpConfigInfo` real answers under the host provider.
3. prepo: hand both buffers to the provider's `report` hook (the event payload is in the second one).
4. infra (napple-infra): napple-online needs only `NAPPLE_ONLINE_HTTP` (TCP, behind Caddy with WebSocket), `NAPPLE_ONLINE_DATA`, optional
   `NAPPLE_ONLINE_PUBLIC_HOST`; no UDP ports.

## Integration result
- Date 2026-11-02, merged into main as a979903. Landed head **193474f** (the orchestrator moved the head from f7c5b68 to 193474f during integration; the
  delta is 7dde717, the ssl IoMode / ResultWouldBlock change napple-online listed under Needs, plus docs). So no separate ssl Small fix was needed.
  Order: after net-rooms and mk8d-lan-session (mk8d was added to the scope mid-run and landed first; this branch was re-merged on top and re-tested).
- Conflicts, mechanical: `Cargo.toml` members (napple-room + napple-online), `runtime/Makefile` (net-room and net-online test blocks both kept),
  `docs/archive/changes/README.md` r7 rows. `offline.c` merged cleanly with mk8d-lan-session's nifm / bsd rows (ssl functions only here).
  `Cargo.lock` auto-merged; `cargo build --workspace --locked` ok. Outcome: **Clean** plus one doc commit (guides index row for `napple-online.md`, r7
  change-archive row, and the line in `docs/guides/network-providers.md` pointing to the Napple-only service, as "How to integrate" asked).
- No native ports: the glue is BSD sockets + the host provider's SecureTransport; the server is portable Rust. C left as written.
- `cargo test -p napple-online`: 31 passed. `cargo build --release -p napple-online` ok.
- `make -C runtime GFX=napple test-net-online`: test_net_online_client 45/0, test_net_online_ssl 42/0 (also re-run on top of mk8d-lan-session's errno change).
  `make -C runtime GFX=napple test test-services test-ipc-reference`: rc 0, ipc_reference 111 / 1325 / missing 0. No new build warnings.
- **MK8D Online Play through a local napple-online (How to test 2), verified twice** (before and after the mk8d-lan-session merge): server log
  `Authentication (10) method 2 -> ok`, `SecureConnection (11) method 1 -> ok`, Utility 7 x2, Ranking 4; runtime log `ID token issued (279 bytes)`,
  `redirected name g2b309e01-lp1.s.n.srv.nintendo.net`, `port 443 -> 127.0.0.1:8737 (http)`; frame 1800 shows Worldwide / Regional / Friends /
  Tournaments.
- Correction to How to test 2: set `NAPPLE_ONLINE_STATE_DIR` to a private directory (or use the same server data directory every run). Without it the
  client keeps its account link in `~/Library/Application Support/Napple/online/<server>/`, and a second run against a fresh server data dir gets
  `POST /napple/v1/token -> 401` ("the server issued no token") and MK8D shows 2001-0123. (The link this run created there was removed afterwards.)
- Four-title 45 s headless boots (on the combined tree with mk8d-lan-session), offline and `NAPPLE_NET=host` without a server: SMO, LAS, MK8D, ACNH rc 0,
  notify-running, `missing_ipc []`, `unsupported_svcs []`, stubbed only `nvdrv:13`; no `napple:` lines without `NAPPLE_ONLINE_SERVER`.
- Still unverified (as declared): the https:// client path against a real TLS server, the server behind Caddy, the server on Linux, matchmaking paths
  other than 40 / 21.2. SessionKeepFailed 2618-0562 is online-pia's (landed next).
