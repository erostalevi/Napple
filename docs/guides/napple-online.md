# Napple online: playing online modes with other Napple users

Napple online is a community-run replacement service for Napple instances only, in the spirit of Pretendo: titles keep their own online code (NEX over
sockets), and everything a console would get from Nintendo comes from a **Napple server** instead. Napple users who point at the same server play
online modes with each other. Mario Kart 8 Deluxe is the first title it targets.

Two parts:
- **Client glue** in the runtime (`runtime/src/net/net_online*.c`), on top of the host network provider (`NAPPLE_NET=host`, see
  [network-providers.md](network-providers.md)). It is switched on by `NAPPLE_ONLINE_SERVER`.
- **The server**, `crates/napple-online` (Rust, binary `napple-online`): Napple-issued accounts and tokens, and a NEX-compatible game server.

## Hard boundaries

These are not configuration options; the code is built around them.

- **Nintendo is never contacted.** With `NAPPLE_ONLINE_SERVER` set, the only name that resolves to something in Nintendo's domains is a title's
  NEX game server (`g<id>-lp1.s.n.srv.nintendo.net`), and it resolves to a reserved virtual address (100.78.78.100) that the runtime connects to
  the Napple server instead. Every other Nintendo name (accounts, dauth/aauth/baas, NAT check `nncs*`, `nintendowifi.net`, ...) and every
  unfilled `%` template answers `EAI_NONAME`.
- **No console keys, device certificates, Nintendo account credentials or Nintendo-issued tokens** are read, used or shipped. The server issues
  its own identities: a Napple account per local user and a Napple-signed token. The title's TLS to "its" game server ends inside the runtime;
  the guest's Nintendo CA list is never used for the Napple server (an `https://` server is verified against the Mac's own trust store under its
  own name).
- **No per-game secrets are committed or needed.** Switch NEX uses PRUDP Lite inside a WebSocket: the packets are not signed with the title's
  access key, and Kerberos tickets are sealed with keys the server chooses (the title is told its source key in the login answer). The server
  therefore never needs anything from the game files.
- **No anti-cheat or ban-evasion features, no eShop or purchase flows.** Play reports stay on the Mac (dropped; shown only with
  `NAPPLE_ONLINE_DEBUG=1`).
- Original code. The public NEX documentation (the NintendoClients wiki, Pretendo's docs) was used for message layouts only.

## Quick start (one Mac, or a LAN)

```bash
cargo build --release -p napple-online
target/release/napple-online serve --listen 0.0.0.0:8737
```

On every Mac that plays:

```bash
NAPPLE_NET=host NAPPLE_ONLINE_SERVER=http://<server address>:8737 runtime/build/napple-runtime <title data dir>
```

| Variable | Meaning |
|---|---|
| `NAPPLE_NET=host` | the host network provider (real sockets, DNS, TLS, nifm); required |
| `NAPPLE_ONLINE_SERVER` | `http://host[:port]` (plain, LAN / same Mac), `https://host[:port]` (a public server behind a TLS proxy), or `default` = `https://pretend.napple.lat`, the community server. Unset = no Napple online (r6 behaviour); it is always opt-in |
| `NAPPLE_ONLINE_STATE_DIR` | where account credentials are kept; default `<support dir>/online/<host_port>/<uid>.txt` (mode 0600) |
| `NAPPLE_ONLINE_DEBUG=1` | logs every lookup, connection, TLS step and (dropped) play report of the glue |

Server configuration (flags win over environment variables; a container needs only `napple-online serve`):

| Flag | Variable | Meaning |
|---|---|---|
| `--listen ADDR:PORT` | `NAPPLE_ONLINE_HTTP` | the one TCP listener: account/token API, `GET /healthz` (200 `ok`), `GET /napple/v1/info`, and the NEX game server (PRUDP Lite inside WebSocket). Default `127.0.0.1:8737` |
| `--data DIR` | `NAPPLE_ONLINE_DATA` | `accounts.json` and `server.key`, the only state the server keeps. Default `~/Library/Application Support/Napple/online-server` |
| `-v` | `NAPPLE_ONLINE_VERBOSE=1` | every call and every packet in hex |
| | `NAPPLE_ONLINE_PUBLIC_HOST` | the name clients use (reported by `/napple/v1/info`) |
| | `NAPPLE_ONLINE_NEX_AUTH`, `NAPPLE_ONLINE_NEX_SECURE`, `NAPPLE_ONLINE_PUBLIC_IP` | accepted and ignored (logged): Switch titles reach NEX through the WebSocket on the HTTP listener and the station URLs name "the same server", so no UDP ports or public IP are needed |

Logs go to stdout. Behind a reverse proxy the client address is taken from the first `X-Forwarded-For` entry. The crate needs only std,
`serde`/`serde_json` and `sha2`: no TLS library, nothing macOS-specific.

### A public server

Run `napple-online serve --listen 127.0.0.1:8737` behind any TLS reverse proxy that forwards WebSocket upgrades (Caddy's `reverse_proxy` does
by default; nginx needs the `Upgrade` / `Connection` headers), with a normal
public certificate for its name, and give players `NAPPLE_ONLINE_SERVER=https://<name>`. The runtime verifies that certificate against the Mac's
trust store, so a server needs no Napple-specific certificate and the account secret never crosses the internet in clear. Keep `--data` private:
`server.key` signs every token and seals every ticket.

## How a title gets online (MK8D, traced on update v1376256)

The path below was traced with `NAPPLE_NET=host NAPPLE_ONLINE_SERVER=http://127.0.0.1:8737 NAPPLE_TRACE_IPC=all NAPPLE_ONLINE_DEBUG=1`, menu
input `15:A,19:DOWN,20.5:DOWN,22:A` (Online Play, 1 player) and a save past the first-boot Mii selection.

| Step | Service calls (guest) | What Napple does |
|---|---|---|
| boot | `acc:u0` InitializeApplicationInfo, `IManagerForApplication` CheckAvailability, GetNintendoAccountUserResourceCacheForApplication | the provider's `account_info`: on first use the glue creates a Napple account (`POST /napple/v1/account`) and reports the user linked with the Napple principal id |
| Online Play | `nifm` CreateRequest, SetConnectionConfirmationOption, Submit, GetResult | host provider: connected (the Mac's IP) |
| | `bsd:u` RegisterClient, StartMonitoring | |
| | `IManagerForApplication` EnsureIdTokenCacheAsync (`IAsyncContext` GetSystemEvent, GetResult, HasDone), GetAccountId, LoadIdTokenCacheDeprecated | the provider's `account_id_token`: `POST /napple/v1/token`, a Napple-signed token (JWS, 15 minutes) |
| | `nsd:u` ResolveEx(`g2b309e01-%.s.n.srv.nintendo.net`) | `%` → `lp1` |
| | `sfdnsres` GetAddrInfoRequest, then GetHostByNameRequest (same name) | redirected: 100.78.78.100 |
| | `nifm` GetCurrentIpAddress, GetCurrentNetworkProfile (fails: not implemented, see below) | |
| | `bsd:u` Socket(AF_INET, STREAM), SetSockOpt, Fcntl, Connect(100.78.78.100:443), GetSockOpt, GetPeerName, GetSockName | the connection goes to the Napple server's address and port; getpeername still reports 100.78.78.100:443 |
| | `ssl` CreateContext, CreateConnection, SetSocketDescriptor, SetHostName(the game server), SetVerifyOption, SetSessionCacheMode, SetIoMode(2 = non-blocking), DoHandshake, then Peek / Read / Write | TLS ends in the glue: plain bytes to an `http://` server, real TLS to an `https://` one; non-blocking reads with nothing pending answer ResultWouldBlock |
| | WebSocket: `GET /` with `Host: g2b309e01-lp1.s.n.srv.nintendo.net` | the server answers 101 |
| NEX | PRUDP Lite: SYN / CONNECT on port 1 (authentication), RMC Authentication 2 ValidateAndRequestTicketWithCustomData(pid, AuthenticationInfo{token, ngs 4, type 2}) | token verified, Kerberos ticket sealed with a per-login source key returned as `pSourceKey`, secure station URL `prudps:/address=0.0.0.1;port=1;CID=1;PID=2;sid=2;stream=10;type=2` |
| | SYN / CONNECT on port 2 (secure) with the internal ticket + the Kerberos-encrypted request | ticket checked, CONNECT-ACK carries check + 1 |
| | SecureConnection 1 Register(station URL), DISCONNECT of port 1, Utility 7 GetIntegerSettings(0), (1), Ranking 4 UploadCommonData | public URL with the observed address and RVCID; settings: empty maps; common data kept |
| online menu | | **"Worldwide / Regional / Friends / Tournaments" is shown** |
| Worldwide | character and kart selection, then MatchmakeExtension 40 AutoMatchmakeWithParam_Postpone(AutoMatchmakeParam) | find-or-create: a gathering (game mode 1, 12 places) owned and hosted by the caller |
| | after ~10 s MatchMaking 2 UnregisterGathering, then DISCONNECT | (see status: the title shows "A communication error has occurred.") |

Names MK8D carries (from its own binary, not committed): the NEX game server `g%08x-%%.s.n.srv.nintendo.net` and `g%08x-%%.p.srv.nintendo.net`,
Pia's NAT check servers `nncs1-%.n.n.srv.nintendo.net` / `nncs2-%...` (blocked), and `api-%%.znc.srv.nintendo.net` (blocked). NEX for_NX 4.3.1.

## The server

`crates/napple-online/src`:

| File | What |
|---|---|
| `accounts.rs` | accounts (`accounts.json`: pid, the uid the instance registered, SHA-256 of its secret), `server.key`, token issue / verify (HS256 JWS, `iss` `napple-online`, `pid`, `tid`, 15 min) |
| `http.rs` | one TCP port: the account API (`/napple/v1/account`, `/napple/v1/token`, `/napple/v1/info`) and the WebSocket upgrade (RFC 6455 framing) |
| `prudp.rs` | PRUDP Lite codec and the server side of a connection: SYN / CONNECT, reliable ordered delivery, single and aggregate acks, fragments, retransmission with backoff, ping, disconnect |
| `kerberos.rs` | RC4 + HMAC-MD5 blobs, client tickets, the server-only internal ticket, the secure CONNECT check |
| `rmc.rs`, `nex.rs` | RMC framing, NEX types (String, Buffer, List, Structure with version header, AnyDataHolder, DateTime, StationURL) |
| `matchmaking.rs` | Gathering / MatchmakeSession (NEX 4.x layout, unknown trailing fields passed through), search criteria, find-or-create, join / leave / owner migration |
| `server.rs` | Authentication (10), SecureConnection (11), MatchmakeExtension (109), MatchMaking (21), MatchMakingExt (50), NATTraversal (3, probe requests forwarded), Health (18), Utility (110), Ranking (112, common data kept, empty rankings), Notification (14, participant joined / left / ownership changed sent to clients); anything else answers Core::NotImplemented |
| `session.rs` | one WebSocket: ports 1 and 2, RMC dispatch, notifications, timers |
| `crypto.rs` | MD5, SHA-1, HMAC, RC4, base64, hex (SHA-256 from `sha2`) |

`cargo test -p napple-online` runs the unit tests, including a full login (SYN → CONNECT → token login → ticket → secure CONNECT → Register) and
two clients matched into one room over PRUDP, in process.

## Status (2026-11-02, r7)

| Phase | Status |
|---|---|
| 1. Trace MK8D's online path | verified (table above) |
| 2. Server unit tests | tested (`cargo test -p napple-online`) |
| 3. Client: Napple tokens, game-server name redirected | verified on MK8D (run log) |
| 4. MK8D connects, logs in, reaches the online menu | verified on MK8D (needs the ssl IoMode / ResultWouldBlock change of this branch and the bsd Poll timeout fix of net-host-provider) |
| 4. Worldwide matchmaking / two instances in one room | not reached: after AutoMatchmakeWithParam_Postpone the title shows "A communication error has occurred." (play report: error 2618-0562, `SessionKeepFailed`, a Pia error) before Pia opens any socket |
| 5. Without `NAPPLE_ONLINE_SERVER`: unchanged | tested (unit test; runtime tests) |

Not implemented: DataStore (115), the MK8D tournament ("Competition") and SimpleSearchObject methods, real rankings, friends / presence through
the server, the `.p.srv` sibling host's protocol, PRUDP V0/V1 over UDP (not used by Switch titles).
