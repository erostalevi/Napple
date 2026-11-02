---
name: napple-online
batch: 2026-11-02-r7
branch: claude/napple-online-2026-11-02-r7
base: claude/net-host-provider-2026-11-02-r6 (merge it again once handed off; lands after it)
env: mac
model: claude-opus-5-5
depends_on: [net-host-provider]
status: handed-off
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_8e7ca377-aa50-49b2-bdc8-9625b3743e48 (fresh)
launched_at: 2026-11-02
---

# napple-online: a Pretendo-style Napple online service so Napple users play online modes with each other (MK8D first)

## Goal
Pretendo-like replacement online for Napple instances only. Games keep their own online code (NEX over sockets); Napple supplies everything that would come from Nintendo, pointing at a **Napple server** instead:
- **Client side (runtime, HLE)**: account/token services return identities and tokens minted by the Napple server (a Napple account per local user, created on first use); network service discovery (nsd) and DNS map the game's server hostnames to the configured Napple server via the `net_host_redirect_lookup` hook net-host-provider added; everything else Nintendo stays blocked.
- **Server side** (`crates/napple-online/`, Rust): account/token endpoints for Napple-issued tokens, and a NEX-compatible game server for MK8D: PRUDP transport (the version MK8D's NEX client uses), RMC framing, Authentication (ticket granting) and Secure connection protocols, Matchmake/MatchmakeExtension enough for MK8D's worldwide/friend-less public lobbies, plus Ranking/DataStore as stubs that answer "empty" cleanly. Use public protocol documentation (Kinnay's NintendoClients wiki, Pretendo's open docs) as references for layouts only — write original code; do not copy GPL code.

Hard boundaries (do not cross, document them in the guide):
- Never contact Nintendo servers; never use or ship console-unique keys, device certificates, Nintendo account credentials or Nintendo-issued tokens. The Napple server issues its own identities.
- Per-game secrets the game itself contains (e.g. a NEX access key) are read from the user's own game at runtime/setup time and never committed or shipped.
- No anti-cheat/ban-evasion features; no eShop/purchase flows.

## Owned paths
`crates/napple-online/` (new; workspace `Cargo.toml` members append only), `runtime/src/net/net_online*.{c,h}` (client glue, redirect table), `runtime/src/services/account*.c` + `online_core.c` (provider ONLINE_CAP_ACCOUNT path only), the nsd rows in `offline.c` ONLY via the provider seam (prefer provider changes over service edits), `runtime/tests/test_net_online*.c`, `docs/guides/napple-online.md` (new: architecture, boundaries, deployment), update the "This repository contains NO online service implementation" note in `runtime/src/services/online_provider.h` to describe the Napple-only scope, own change note.

## Do not touch
net-rooms' paths (`crates/napple-room/`, `net_room*`, LDN/LAN relay hooks), `gfx/`, `applet*.c`, `orchestrations/`.

## Acceptance (phased; hand off with honest status per phase)
1. Trace: MK8D update, Online Play with `NAPPLE_NET=host` + Nintendo hosts blocked — list every service call, hostname and socket the game uses to go online (the protocol path MK8D takes), committed as part of the guide.
2. Server: `cargo test -p napple-online` covering token issue/verify, PRUDP handshake/ack/retransmit, RMC encode/decode, Authentication login + ticket, Secure register.
3. Client: with `NAPPLE_ONLINE_SERVER=<url>` MK8D obtains Napple tokens and resolves its game-server name to the Napple server (verified in a run log).
4. **Verified on a title**: MK8D connects to a local `napple-online` server, completes login, reaches the online menu/lobby; stretch: two MK8D instances on the same server are matched into the same room.
5. Without `NAPPLE_ONLINE_SERVER`, behaviour equals r6; runtime tests green; four-title boots no regression.

## Env notes
Read `orchestrations/ENV-MAC.md` (shared data read-only, locks, local-only, never ask). Two simultaneous instances: one job under the run lock, ≤120 s.

## Result
