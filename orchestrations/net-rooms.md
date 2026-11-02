---
name: net-rooms
batch: 2026-11-02-r7
branch: claude/net-rooms-2026-11-02-r7
base: claude/net-host-provider-2026-11-02-r6 (merge it again once handed off; lands after it)
env: mac
model: claude-opus-5-5
depends_on: [net-host-provider]
status: launched
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_d8199ac5-b4e7-45f0-8d48-22e3a3ab703e (fresh)
launched_at: 2026-11-02
---

# net-rooms: local wireless (LDN) and LAN Play between Napple instances over the internet

## Goal
Let any two Napple users play local-multiplayer modes together over the internet, without Nintendo servers:
1. **LDN rooms**: net-host-provider (r6) gives LDN over the local LAN. Add a relay transport: instances join a named room on a Napple room server; LDN scan/create/connect/data frames are tunnelled through it (yuzu-multiplayer-room style).
2. **LAN Play relay**: titles' "LAN Play" modes (e.g. MK8D L+R+stick on the title) use plain UDP broadcast/unicast through bsd sockets on a wired network. Give each instance a virtual address in a private subnet (e.g. 10.13.0.0/16, configurable) inside the host provider; guest sockets bound to that interface (and nifm's reported IP) are tunnelled through the room server, which forwards unicast and fans out broadcast (switch-lan-play style).
3. **Room server**: a small standalone server in this repo (`crates/napple-room/`, Rust, tokio or std) — rooms with optional password, member list, keepalive, rate limits, max members, relay of LDN frames and virtual-LAN packets; runs on macOS/Linux; documented deployment.
Our own protocol only; nothing Nintendo-specific beyond the LDN/LAN data the games already send.

## Owned paths
`crates/napple-room/` (new; add to workspace `Cargo.toml` members — append only), `runtime/src/net/net_room*.{c,h}` (client), small hooks in `runtime/src/net/net_host_ldn.c` / `net_host_sock.c` (transport selection), `runtime/tests/test_net_room*.c`, `docs/guides/online-rooms.md` (new), `docs/reference/environment-variables.md` (append `NAPPLE_ROOM`, `NAPPLE_ROOM_PASSWORD`, `NAPPLE_VLAN`), own change note. Studio UI is out of scope (note what Studio would need under "Needs from other areas").

## Do not touch
`runtime/src/services/offline*.c`, `account*.c`, `nsd`/acc auth paths (napple-online owns those), `gfx/`, `crates/napple-online/`, `orchestrations/`.

## Acceptance
1. `cargo test -p napple-room` (room lifecycle, password, broadcast fan-out, LDN frame relay, limits).
2. Two runtime instances on this Mac, each pointing at a local `napple-room` server: LDN scan sees the other's network, join succeeds, data flows (test harness or two headless runs).
3. **Verified on a title**: two headless MK8D (update) instances via the room server reach the same LAN Play lobby (each sees the other) — use `NAPPLE_INPUT_SCRIPT` for both; screenshot/log evidence. If MK8D LAN Play turns out to use LDN instead of sockets, verify through the LDN path instead.
4. Without `NAPPLE_ROOM` everything behaves as in r6 (tests unchanged).
5. `make -C runtime test test-services test-ipc-reference` green; four-title boots no regression.

## Env notes
Read `orchestrations/ENV-MAC.md` (shared data read-only, locks, local-only, never ask). Two simultaneous game instances need ~2x memory: run them as ONE job under the run lock, ≤120 s.

## Result
