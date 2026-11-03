# Napple online: online modes with other Napple users

Napple online is a community-run replacement service for Napple instances only, in the spirit of Pretendo: titles keep their own online code (NEX over
sockets), and everything a console would get from Nintendo comes from a **Napple server** instead. Napple users who point at the same server play
online modes with each other.

Two parts:
- **Client glue** in the runtime (`runtime/src/net/net_online*.c`), on top of the host network provider (`NAPPLE_NET=host`, see
  [network-providers.md](network-providers.md)). It is switched on by `NAPPLE_ONLINE_SERVER`.
- **The server**, `crates/napple-online` (Rust, binary `napple-online`): accounts and tokens (its own, or Napple Account's) and a NEX-compatible
  game server.

The rest of Napple Online: [Napple Account](napple-account.md) (sign-in, profiles, tiers), [napple-social](napple-social.md) (friends, presence),
[cloud saves](cloud-saves.md), the [client core](napple-online-client.md) behind the overlay's Online tab, [rooms](online-rooms.md) for local
wireless and LAN play, [the end-to-end test](online-e2e.md) and [deployment](deploy-servers.md). Per-title online status is in the generated
[online support](../reference/online-support.md) table and the [title pages](../titles/README.md).

## Hard boundaries

These are not configuration options; the code is built around them.

- **Nintendo is never contacted.** With `NAPPLE_ONLINE_SERVER` set, the only name that resolves to something in Nintendo's domains is a title's
  NEX game server (`g<id>-lp1.s.n.srv.nintendo.net`), and it resolves to a reserved virtual address (100.78.78.100) that the runtime connects to
  the Napple server instead. Every other Nintendo name (accounts, dauth/aauth/baas, NAT check `nncs*`, `nintendowifi.net`, ...) and every
  unfilled `%` template answers `EAI_NONAME`; the Pia NAT-check names are filled and redirected to the Napple server.
- **No console keys, device certificates, Nintendo account credentials or Nintendo-issued tokens** are read, used or shipped. The server issues
  its own identities. The title's TLS to "its" game server ends inside the runtime; the guest's Nintendo CA list is never used for the Napple
  server (an `https://` server is verified against the Mac's own trust store under its own name).
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
| `NAPPLE_ONLINE_SERVER` | `http://host[:port]` (plain, LAN / same Mac), `https://host[:port]` (a public server behind a TLS proxy), or `default` = `https://pretend.napple.lat`, the community server. Unset = no Napple online; it is always opt-in |
| `NAPPLE_ONLINE_STATE_DIR` | where self-issued account credentials are kept; default `<support dir>/online/<host_port>/<uid>.txt` (mode 0600) |
| `NAPPLE_ONLINE_DEBUG=1` | logs every lookup, connection, bind, NAT-check datagram, TLS step and (dropped) play report of the glue, with the guest call stack for reports |

Server configuration (flags win over environment variables; a container needs only `napple-online serve`):

| Flag | Variable | Meaning |
|---|---|---|
| `--listen ADDR:PORT` | `NAPPLE_ONLINE_HTTP` | the one TCP listener: account/token API, `GET /healthz` (200 `ok`), `GET /napple/v1/info`, and the NEX game server (PRUDP Lite inside WebSocket). Default `127.0.0.1:8737` |
| `--data DIR` | `NAPPLE_ONLINE_DATA` | `accounts.json`, `server.key` and `matchlog/`, the only state the server keeps. Default `~/Library/Application Support/Napple/online-server` |
| `-v` | `NAPPLE_ONLINE_VERBOSE=1` | every call and every packet in hex |
| | `NAPPLE_ONLINE_PUBLIC_HOST` | the name clients use (reported by `/napple/v1/info`) |
| | `NAPPLE_ONLINE_NATCHECK_BASE` | the NAT check titles run before matchmaking: UDP 10025, 10125, 33334, 33335 (primary server) and the same + 1000 (secondary). Default 10025, `0` = off |
| | `NAPPLE_ONLINE_PUBLIC_IP` | the address reported for clients on the server's own machine (else the machine's LAN address) |
| | `NAPPLE_ACCOUNT_URL` | napple-account base URL; set = Napple Account identity mode (below) |
| | `NAPPLE_SOCIAL_URL`, `NAPPLE_SOCIAL_SERVICE_TOKEN` | napple-social base URL and service credential for match reporting |
| | `NAPPLE_ONLINE_ADMIN_ACCOUNTS` | account ids allowed to call the admin endpoints |
| | `NAPPLE_ONLINE_NEX_AUTH`, `NAPPLE_ONLINE_NEX_SECURE` | accepted and ignored (logged): Switch titles reach NEX through the WebSocket on the HTTP listener |

Logs go to stdout. Behind a reverse proxy the client address is taken from the first `X-Forwarded-For` entry. The crate needs no TLS library and
nothing macOS-specific.

### A public server

Run `napple-online serve --listen 127.0.0.1:8737` behind any TLS reverse proxy that forwards WebSocket upgrades (Caddy's `reverse_proxy` does
by default; nginx needs the `Upgrade` / `Connection` headers), with a normal public certificate for its name, and give players
`NAPPLE_ONLINE_SERVER=https://<name>`. The runtime verifies that certificate against the Mac's trust store, so a server needs no Napple-specific
certificate and the account secret never crosses the internet in clear. Keep `--data` private: `server.key` signs every self-issued token and
seals every ticket. The public deployment is described in [deploy-servers.md](deploy-servers.md).

## Identity

The server runs in one of two modes.

**Self-issued (LAN / development, `NAPPLE_ACCOUNT_URL` unset).** On first use the glue creates a Napple account per local user
(`POST /napple/v1/account`) and gets a Napple-signed token (`POST /napple/v1/token`, HS256 JWS, `iss` `napple-online`, 15 minutes).

**Napple Account (`NAPPLE_ACCOUNT_URL=http://127.0.0.1:<port>`, the local napple-account listener; plain `http://`, TLS ends at the proxy).**
The server accepts **napple-account access JWTs** instead, and refuses self-issued tokens and `/napple/v1/{account,token,friends}` (410).

- **Login.** The NEX token is the access JWT (Ed25519, `alg` EdDSA/Ed25519, `aud` `napple`, `sub` account UUID, `did`). Keys come from
  `/account/v1/.well-known/jwks.json`, cached 10 min; an unknown `kid` re-fetches at most every 10 s; a known key keeps working while the account
  service is down; a key no longer published is refused after the next refresh. The NEX login name may carry the acting profile and title:
  `<profile_uuid>` or `<profile_uuid>/<title_id16>`; otherwise the account's first profile and title `unknown`. The profile must belong to the
  account (`GET /account/v1/profiles` with the caller's own token, cached 60 s).
- **Principal id.** `pid = 0x4e41_8000_0000_0000 | (SHA-256("napple-online/pid/v1:" + profile_id)[0..6] & 0x7fff_ffff_ffff)`: the same profile has
  the same pid on every device and run; bit 47 keeps it apart from self-issued counter pids; it reveals nothing about the account.
- **Names.** The game sees the profile nickname; at visibility tier 5 (Ghost) a random alias like `QuietFalcon42`, created at login and kept until
  that player's last connection ends. Aliases are unique among live players and never equal a live real nickname.
- **Match log.** Every match records, for each participant (ghosts included): account, profile, pid, tier, nickname, the name the game showed,
  title, join/leave times, match id. Closed matches go to `<data>/matchlog/matchlog.jsonl`; an hourly job purges those that ended more than 30 days
  ago. Admin endpoints (bearer = access JWT of an account listed in `NAPPLE_ONLINE_ADMIN_ACCOUNTS`): `POST /online/reports {alias, match_id?}`
  returns the real account/profile behind the alias (404 if none), `GET /online/matchlog/export?since=<unix>`.
- **Social reporting.** With `NAPPLE_SOCIAL_URL` and `NAPPLE_SOCIAL_SERVICE_TOKEN`, when a match ends: `POST /social/v1/encounters {profile_ids,
  title_id, match_id}` (also the shared-lobby proof for tier-3 friend requests) and per participant `POST /social/v1/internal/stats {profile_id,
  title_id, deltas:{matches_played, play_seconds}}`, with the service token in the `X-Napple-Service` header. Ghost players are never included.
  Failed posts are retried (bounded outbox, 8 attempts).

## How a title gets online

The path a NEX title takes, and what Napple answers at each step:

| Step | Service calls (guest) | What Napple does |
|---|---|---|
| boot | `acc:u0` InitializeApplicationInfo, `IManagerForApplication` CheckAvailability, GetNintendoAccountUserResourceCacheForApplication | the provider's `account_info`: the user is reported as linked with its Napple principal id |
| online menu | `nifm` CreateRequest, SetConnectionConfirmationOption, Submit, GetResult | host provider: connected (the Mac's IP) |
| | `bsd:u` RegisterClient, StartMonitoring | |
| | `IManagerForApplication` EnsureIdTokenCacheAsync (`IAsyncContext` GetSystemEvent, GetResult, HasDone), GetAccountId, LoadIdTokenCacheDeprecated | the provider's `account_id_token`: a self-issued token or the Napple Account access token |
| | `nsd:u` ResolveEx(`g<id>-%.s.n.srv.nintendo.net`) | `%` -> `lp1` |
| | `sfdnsres` GetAddrInfoRequest / GetHostByNameRequest (same name) | redirected: 100.78.78.100 |
| | `bsd:u` Socket(AF_INET, STREAM), SetSockOpt, Fcntl, Connect(100.78.78.100:443), GetSockOpt, GetPeerName, GetSockName | the connection goes to the Napple server's address and port; getpeername still reports 100.78.78.100:443 |
| | `ssl` CreateContext, CreateConnection, SetSocketDescriptor, SetHostName(the game server), SetVerifyOption, SetSessionCacheMode, SetIoMode(2 = non-blocking), DoHandshake, then Peek / Read / Write | TLS ends in the glue: plain bytes to an `http://` server, real TLS to an `https://` one; non-blocking reads with nothing pending answer ResultWouldBlock |
| | WebSocket: `GET /` with `Host: g<id>-lp1.s.n.srv.nintendo.net` | the server answers 101 |
| NEX login | PRUDP Lite: SYN / CONNECT on port 1 (authentication), RMC Authentication 2 ValidateAndRequestTicketWithCustomData(pid, AuthenticationInfo{token, ngs 4, type 2}) | token verified, Kerberos ticket sealed with a per-login source key returned as `pSourceKey`, secure station URL `prudps:/address=0.0.0.1;port=1;CID=1;PID=2;sid=2;stream=10;type=2` |
| | SYN / CONNECT on port 2 (secure) with the internal ticket + the Kerberos-encrypted request | ticket checked, CONNECT-ACK carries check + 1 |
| | SecureConnection 1 Register(station URL), Utility, Ranking calls | public URL with the observed address and RVCID; settings: empty maps; common data kept |
| matchmaking | MatchmakeExtension AutoMatchmake* | find-or-create a gathering owned and hosted by the caller; verbose gatherings (flags 0x200 / 0x800) announce every join, the joiner's own included (Notification 3001) |
| NAT check | `sfdnsres` (`nncs1-%.n.n.srv.nintendo.net`, `nncs2-%...`), `bsd` UDP Socket, Bind, probe packets | names filled and redirected; the server answers on UDP 10025 / 10125 (nncs1) and 11025 / 11125 (nncs2) |
| | SecureConnection 7 ReplaceURL (station URL with `natm` / `natf`) | |

The per-title traces (names a title carries, menu inputs, what each title reaches) are on the [title pages](../titles/README.md).

## The server

`crates/napple-online/src`:

| File | What |
|---|---|
| `accounts.rs` | self-issued accounts (`accounts.json`: pid, the uid the instance registered, SHA-256 of its secret), `server.key`, token issue / verify (HS256 JWS, `iss` `napple-online`, `pid`, `tid`, 15 min) |
| `http.rs` | one TCP port: the account API (`/napple/v1/account`, `/napple/v1/token`, `/napple/v1/info`) and the WebSocket upgrade (RFC 6455 framing) |
| `prudp.rs` | PRUDP Lite codec and the server side of a connection: SYN / CONNECT, reliable ordered delivery, single and aggregate acks, fragments, retransmission with backoff, ping, disconnect |
| `kerberos.rs` | RC4 + HMAC-MD5 blobs, client tickets, the server-only internal ticket, the secure CONNECT check |
| `rmc.rs`, `nex.rs` | RMC framing, NEX types (String, Buffer, List, Structure with version header, AnyDataHolder, DateTime, StationURL) |
| `matchmaking.rs` | Gathering / MatchmakeSession (NEX 4.x layout, unknown trailing fields passed through), search criteria, find-or-create, join / leave / owner migration |
| `server.rs` | Authentication (10), SecureConnection (11), MatchmakeExtension (109), MatchMaking (21), MatchMakingExt (50), NATTraversal (3, probe requests forwarded), Health (18), Utility (110), Ranking (112, common data kept, empty rankings), Notification (14, participant joined / left / ownership changed sent to clients); anything else answers Core::NotImplemented |
| `session.rs` | one WebSocket: ports 1 and 2, RMC dispatch, notifications, timers |
| `crypto.rs` | MD5, SHA-1, HMAC, RC4, base64, hex (SHA-256 from `sha2`) |

Not implemented: DataStore (115), tournament and SimpleSearchObject methods, real rankings, friend requests sent from a game, profile images in
game, the `.p.srv` sibling host's protocol, PRUDP V0/V1 over UDP (not used by Switch titles).

## Friends (napple-social)

With a Napple Account token the online wrapper also serves `friend:u` from [napple-social](napple-social.md):

| Variable | Meaning |
|---|---|
| `NAPPLE_ACCOUNT_TOKEN_FILE` / `NAPPLE_ACCOUNT_TOKEN` | a napple-account access token (the [client core](napple-online-client.md) provides it through `napple_account_access_token()` when the Mac is signed in) |
| `NAPPLE_ACCOUNT_URL` | the napple-account server (for the profile list) |
| `NAPPLE_SOCIAL_URL` | the napple-social server |
| `NAPPLE_SOCIAL_PROFILE` | the profile to act as (default: the profile mapped to the local user) |
| `NAPPLE_SOCIAL_POLL_MS` | friend-list poll interval (default 5000) |
| `NAPPLE_FRIEND_LOG=1` | log every friend:u call |

Friends who play on the same napple-online server appear under their NEX pid, so game-server lookups by friend work; the runtime reports its
friends' pids to napple-online (`POST /napple/v1/friends`), which answers GetPlayingSession from them. Presence is "playing <title>" with a join
token `np1:<server key>:<pid>:<app data>`.

## Tests

`cargo test -p napple-online` runs the unit tests: a full login (SYN -> CONNECT -> token login -> ticket -> secure CONNECT -> Register), two
clients matched into one room over PRUDP in process, and (with mock JWKS / account / social servers) `tests/hub_server.rs`: login, ghost
aliases, match log and reporting through the public server API. Without `NAPPLE_ONLINE_SERVER` the runtime behaves exactly as with the plain host
provider (unit and runtime tests).
