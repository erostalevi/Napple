# Online rooms: local wireless and LAN play over the internet

Napple rooms let Napple users play a title's local multiplayer modes together from different places:

- **Local wireless (LDN)**, the "Wireless Play" / "Local Play" menus. A console hosts a network in the room and the others find it and join it, as
  if they were in the same living room.
- **LAN play**, for the titles that have a wired-LAN mode (Mario Kart 8 Deluxe: L+R+left stick on the main menu turns "Wireless Play" into "LAN
  Play"). The room acts as one virtual Ethernet segment: every member gets an address in a private subnet, and broadcasts reach every other member.

A small server, `napple-room` (`crates/napple-room/`), relays the traffic between the members of a room. The protocol is Napple's own. Nothing here
talks to, imitates or needs any Nintendo service: the runtime puts the LDN and LAN data the games already send inside room frames.

## Playing

1. Someone runs a room server that every player can reach (see [Running a server](#running-a-server)).
2. Every player starts their game with the host network provider and the same room:

   ```bash
   NAPPLE_NET=host NAPPLE_ROOM=rooms.example.net/friday NAPPLE_ROOM_PASSWORD=secret NAPPLE_ROOM_NICK=neto \
     runtime/build/napple-runtime <data-dir>
   ```

   `NAPPLE_ROOM` is `<host>[:<port>][/<room>]`; the port defaults to 31415 and the room to `lobby`. The first player in a room sets its password;
   the others must give the same one. The log shows `net: room: joined 'friday' on rooms.example.net:31415 as 10.13.0.2 ...` and a line for every
   member who joins or leaves.
3. In the game, use Wireless Play or LAN Play as on a console. One player creates a room (LDN) or a LAN session; the others see it in their lists.

Several players on one Mac work too (each runtime has its own virtual address). That is how the tests run.

The variables are listed in [environment variables](../reference/environment-variables.md#napple-rooms-net-rooms-2026-11-02-r7).

## Running a server

```bash
cargo build --release -p napple-room
target/release/napple-room --listen 0.0.0.0:31415
```

Options (defaults in brackets): `--listen ADDR` [0.0.0.0:31415], `--subnet CIDR` [10.13.0.0/16; the virtual LAN; `.1` is kept for a gateway],
`--max-members N` [16 per room], `--max-rooms N` [256], `--max-clients N` [1024 connections], `--rate-kbps N` [2048 relayed KiB/s per member],
`--rate-pps N` [4000 relayed packets/s per member], `--keepalive SECS` [30: a member silent that long is dropped; clients ping every 5 s], `--quiet`.

`napple-room list HOST[:PORT]` prints the rooms of a server (name, members, whether a password is set).

Deployment notes:

- One TCP port. Open it in the firewall (and forward it on the router if the server is at home). Players only make outgoing connections, so they need
  no port forwarding.
- The server is a single process with no state on disk: rooms live while they have members. Run it under launchd / systemd with restart on failure.
- Bandwidth: everything a member sends is relayed through the server, once per recipient for broadcasts. Mario Kart LAN races are some tens of KiB/s
  per player; plan for the rate limits above times the members.
- Latency adds the trip to the server: pick a server close to all players.
- The server never reads the relayed data, but anyone who knows a room's name (and its password) can join it. Use a password for private games.
  Traffic is not encrypted.

## How it works

```
 Napple A ─┐                                       ┌─ Napple B
 (10.13.0.2)│  TCP: frames (HELLO, PACKET, LDN ...) │(10.13.0.3)
            └────────────► napple-room ◄────────────┘
```

**Server** (`crates/napple-room/src/server.rs`): a client sends HELLO (room, password, nickname, wanted address) and gets WELCOME (its member id, its
virtual address and the subnet mask, the member list). The server then relays PACKET frames: the source must be the sender's own address (anything
else is dropped); the destination is one member, or every other member when it is the subnet broadcast or 255.255.255.255. Members advertise the LDN
network they host (LDN_ADVERTISE) and ask for the others' (LDN_SCAN / LDN_SCAN_RESULT). MEMBER_JOIN / MEMBER_LEAVE tell everyone about changes; PING /
PONG keep the connection alive. Each member has its own send queue: a slow member loses relayed packets (a LAN drops them too) instead of slowing the
others. Frame layout: `crates/napple-room/src/proto.rs`.

**Runtime** (`runtime/src/net/net_room*.c`, inside the host provider):

- `net_room.c`: the connection to the server (one per process), a worker thread, an outgoing queue that never blocks a game thread, reconnects every
  3 s after a loss (asking for the same address).
- `net_room_vlan.c`: the virtual LAN. nifm reports the virtual address as a wired connection. A guest socket bound to INADDR_ANY or to the virtual
  address, or sending to a virtual address or a broadcast, joins the virtual LAN. It stays a real host socket, bound to an ephemeral port (so every
  instance on one Mac can "bind" port 49152). Datagrams from the room are injected into it over loopback with a small header naming their virtual
  source, which the receive path strips. A TCP connect to a virtual address asks the peer first, then is spliced through loopback connections. Blocking,
  timeouts and poll work unchanged. A broadcast also reaches the sender's own sockets, as on a BSD stack.
- `net_room_ldn.c`: LDN. A hosted network is advertised to the server, a scan asks the server, and the JOIN / ACCEPT / NODES / BYE session of the LAN
  transport travels as room packets. Node addresses are virtual addresses, so the game's own sockets reach the other nodes over the virtual LAN. A
  member that leaves the room leaves the network; the host leaving ends it.

Without `NAPPLE_ROOM`, none of this runs and the host provider behaves exactly as before (LDN over the real LAN, real addresses).

## Status (2026-11-02)

- Server: `cargo test -p napple-room` (13 tests: room life cycle, passwords, addresses, unicast / broadcast relay, spoofing, LDN advertise / scan /
  relay, limits, rate limit, keepalive).
- Runtime: `make -C runtime test-net-room` runs a real server and two consoles (two processes) through the provider: nifm, UDP on the same port from
  both, broadcast, unicast, implicit bind, connected UDP, TCP with 100 kB both ways, LDN scan / join / nodes / advertise / leave / destroy, a member
  quitting.
- Mario Kart 8 Deluxe (update 3.0.5), two headless instances on one Mac in one room, LAN Play: both bind 49152 / 40000 / 30000 on their own virtual
  addresses, and each receives the other's LAN session announcement from 10.13.0.x. The title then stops with error 2618-0006 about 2 s later. It does
  the same with one instance on the real LAN without rooms, and in Wireless Play (LDN). That is a runtime gap outside rooms, still being
  investigated; see the net-rooms integrator file. LAN Play also needs nifm `GetCurrentIpConfigInfo`, which the offline nifm service still stubs
  (a patch is in the hand-off).

## Not done yet

- No UDP transport to the server (TCP only: a lost packet delays the ones behind it).
- No NAT traversal or direct peer-to-peer path: everything goes through the server.
- No room browser in Napple Studio: rooms are joined with environment variables.
- TCP between virtual addresses supports connect / accept / data / half-close. TCP to one's own virtual address, IP options and multicast are not
  modelled.
