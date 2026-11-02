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
| `NAPPLE_ONLINE_DEBUG=1` | logs every lookup, connection, bind, NAT-check datagram, TLS step and (dropped) play report of the glue, with the guest call stack for reports |

Server configuration (flags win over environment variables; a container needs only `napple-online serve`):

| Flag | Variable | Meaning |
|---|---|---|
| `--listen ADDR:PORT` | `NAPPLE_ONLINE_HTTP` | the one TCP listener: account/token API, `GET /healthz` (200 `ok`), `GET /napple/v1/info`, and the NEX game server (PRUDP Lite inside WebSocket). Default `127.0.0.1:8737` |
| `--data DIR` | `NAPPLE_ONLINE_DATA` | `accounts.json` and `server.key`, the only state the server keeps. Default `~/Library/Application Support/Napple/online-server` |
| `-v` | `NAPPLE_ONLINE_VERBOSE=1` | every call and every packet in hex |
| | `NAPPLE_ONLINE_PUBLIC_HOST` | the name clients use (reported by `/napple/v1/info`) |
| | `NAPPLE_ONLINE_NATCHECK_BASE` | the NAT check titles run before matchmaking: UDP 10025, 10125, 33334, 33335 (primary server) and the same + 1000 (secondary). Default 10025, `0` = off |
| | `NAPPLE_ONLINE_PUBLIC_IP` | the address reported for clients on the server's own machine (else the machine's LAN address) |
| | `NAPPLE_ONLINE_NEX_AUTH`, `NAPPLE_ONLINE_NEX_SECURE` | accepted and ignored (logged): Switch titles reach NEX through the WebSocket on the HTTP listener |

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
| | Notification 1: 3001 "participant joined" about itself (verbose gathering) | without it MK8D waits 10 s, unregisters and shows SessionKeepFailed |
| NAT check | `sfdnsres` GetAddrInfoRequest(`nncs1-%.n.n.srv.nintendo.net`), (`nncs2-%…`); `bsd` Socket(UDP), Bind; five zero packets to nncs1:33334, then types 101 / 102 to nncs1:10025, 103 to nncs2:10025 | names filled and redirected; the server answers on UDP 10025 / 10125 (nncs1) and 11025 / 11125 (nncs2) |
| | SecureConnection 7 ReplaceURL (station URL with `natm=1;natf=1`) | |
| | NatDetectionJob: Pia packets (`NATTestId_Dummy`) to nncs1:33335 from a fresh socket, closed at once | dropped; then 2618-0006 (open) |

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

## Friends (napple-social)
With a Napple Account token the online wrapper also serves `friend:u` from [napple-social](napple-social.md) (online-friends, r10):

| Variable | Meaning |
|---|---|
| `NAPPLE_ACCOUNT_TOKEN_FILE` / `NAPPLE_ACCOUNT_TOKEN` | a napple-account access token (Studio / napple-cloud can provide it through `napple_account_access_token()`) |
| `NAPPLE_ACCOUNT_URL` | the napple-account server (for the profile list) |
| `NAPPLE_SOCIAL_URL` | the napple-social server |
| `NAPPLE_SOCIAL_PROFILE` | the profile to act as (default: the account's first profile) |
| `NAPPLE_SOCIAL_POLL_MS` | friend-list poll interval (default 5000) |
| `NAPPLE_FRIEND_LOG=1` | log every friend:u call |

Friends who play on the same napple-online server appear under their NEX pid, so game-server lookups by friend work; the runtime reports its
friends' pids to napple-online (`POST /napple/v1/friends`), which answers GetPlayingSession from them. Presence is "playing <title>" with a join
token `np1:<server key>:<pid>:<app data>`. MK8D status: a friend shows as online / joinable and the Friends menu opens, but its friend list stays
empty (open). Not done: friend requests sent from a game, profile images.

## MK8D: two instances into the race lobby (r12)

Two MK8D instances on one Mac reach the race lobby (character, kart, course vote with both players) through a local napple-online **and a local
napple-room**. What it took, each found with `NAPPLE_DEBUG_LOG` breakpoints on the Pia connect job and `NAPPLE_NET_LOG=2` datagram dumps:

| Symptom | Cause | Fix |
|---|---|---|
| the joiner ends with 2618-0502 ~150 ms after NAT traversal, no connection request | Pia's connect step (`ConnectStationJob`, main+0x99de0c; its check main+0x9330e8 -> main+0x94bd88) refuses a station it has not heard of: the join notification (3001) of the host never reached the joiner | napple-online tells a joiner about every participant already in a verbose gathering, before its own join |
| the host answers the connection request with a refusal (result 1) | Pia looks a requesting station up by its IPv4 address only (main+0x9ae624): two consoles at one address (two instances on one Mac) look like one station | a client on the server's own machine gets its console address (nifm; a room gives each instance its own virtual address) as its public address: the runtime puts it in the NAT check message's "local IPv4" field and replaces the reply's external port by the socket's virtual port; the server trusts both only for loopback clients and uses the Register URL's own address |

Run it: `napple-room serve --listen 127.0.0.1:31498`, and per instance `NAPPLE_NET=host NAPPLE_ROOM=127.0.0.1:31498/mk8d NAPPLE_ONLINE_SERVER=...`
(`NAPPLE_ONLINE_NATCHECK_LOCALIP` 1 / 0 forces it; unset it follows the room). Without a room two instances still share the Mac's address and the host
refuses the first joiner: that case needs two Macs.

## Status (2026-11-02, r7)

| Phase | Status |
|---|---|
| 1. Trace MK8D's online path | verified (table above) |
| 2. Server unit tests | tested (`cargo test -p napple-online`) |
| 3. Client: Napple tokens, game-server name redirected | verified on MK8D (run log) |
| 4. MK8D connects, logs in, reaches the online menu | verified on MK8D (needs the ssl IoMode / ResultWouldBlock change of this branch and the bsd Poll timeout fix of net-host-provider) |
| 4. Worldwide matchmaking | SessionKeepFailed (2618-0562) fixed: verbose gatherings (flags 0x200 / 0x800) announce every join, the joiner's own included. Pia's NAT check passes (natm=1, natf=1). Two instances are matched into one gathering and the joiner gets the host's station URL. Still open: 2618-0006 right after Pia's NatDetectionJob, before any peer-to-peer packet (online-pia integrator file) |
| 5. Without `NAPPLE_ONLINE_SERVER`: unchanged | tested (unit test; runtime tests) |

Not implemented: DataStore (115), the MK8D tournament ("Competition") and SimpleSearchObject methods, real rankings, friends / presence through
the server, the `.p.srv` sibling host's protocol, PRUDP V0/V1 over UDP (not used by Switch titles).
