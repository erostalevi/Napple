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

   `NAPPLE_ROOM` is `<host>[:<port>][/<room>]`; the port defaults to 31415 and the room to `lobby`. `NAPPLE_ROOM=default` (or `default/<room>`)
   means the public Napple room server, `pretend.napple.lat:31415`. Rooms stay opt-in: nothing connects anywhere without `NAPPLE_ROOM`. The first player in a room sets its password;
   the others must give the same one. The log shows `net: room: joined 'friday' on rooms.example.net:31415 as 10.13.0.2 ...` and a line for every
   member who joins or leaves.
3. In the game, use Wireless Play or LAN Play as on a console. One player creates a room (LDN) or a LAN session; the others see it in their lists.

Several players on one Mac work too (each runtime has its own virtual address). That is how the tests run.

The variables are listed in [environment variables](../reference/environment-variables.md#napple-rooms-net-rooms-2026-11-02-r7).

## Running a server

```bash
cargo build --release -p napple-room --bin napple-room
NAPPLE_ROOM_LISTEN=0.0.0.0:31415 NAPPLE_ROOM_HTTP=0.0.0.0:8081 target/release/napple-room
```

The server builds from std alone (no system libraries beyond libc), on macOS and Linux. It is configured by environment variables; each one also
has a flag of the same name (`--listen`, `--http`, `--subnet`, ...), and the flag wins:

| Variable | Default | Meaning |
|---|---|---|
| `NAPPLE_ROOM_LISTEN` | `0.0.0.0:31415` | The room protocol, **TCP** only. No UDP port is used yet. |
| `NAPPLE_ROOM_HTTP` | off | Plain-HTTP listener: `GET /healthz` gives `200 ok`; `GET /rooms` gives JSON `{"version", "clients", "rooms": [{"name", "members", "max_members", "password"}]}`. Put TLS in front with a reverse proxy if it is public. |
| `NAPPLE_ROOM_SUBNET` | `10.13.0.0/16` | The virtual LAN of each room (`.1` is kept for a gateway). |
| `NAPPLE_ROOM_MAX_MEMBERS` | 16 | Members per room (2-254). |
| `NAPPLE_ROOM_MAX_ROOMS` | 256 | Rooms on the server. |
| `NAPPLE_ROOM_MAX_CLIENTS` | 1024 | Open connections. |
| `NAPPLE_ROOM_RATE_KBPS` | 2048 | Relayed KiB/s per member (burst a quarter of it). |
| `NAPPLE_ROOM_RATE_PPS` | 4000 | Relayed packets/s per member. |
| `NAPPLE_ROOM_KEEPALIVE` | 30 | Seconds of silence before a member is dropped (clients ping every 5 s; at least 5). |
| `NAPPLE_ROOM_QUIET` | 0 | `1` turns the log off. |
| `NAPPLE_ROOM_DATA` | — | Accepted and unused: rooms live in memory only, nothing is written to disk. |

A bad value stops the server at start with status 2. Logs go to stdout, one line per join or leave. SIGTERM or SIGINT exits with status 0.

Other commands:

- `napple-room health [HOST[:PORT]]` asks the room service for its room list and exits 0 when it answers, 1 otherwise. The default address is
  `127.0.0.1` on the configured listen port, so a container healthcheck can run `napple-room health` with no curl in the image. The HTTP
  `/healthz` serves load balancers.
- `napple-room list HOST[:PORT]` prints the rooms of a server (name, members, whether a password is set).

Deployment notes:

- Open the TCP port in the firewall (and forward it on the router if the server is at home). Players only make outgoing connections, so they need no
  port forwarding.
- The server is a single stateless process: rooms live while they have members. It runs as any unprivileged user from a read-only filesystem.
  Restart it on failure (launchd, systemd or the container runtime).
- Bandwidth: everything a member sends is relayed through the server, once per recipient for broadcasts. Mario Kart LAN races are some tens of KiB/s
  per player; plan for the rate limits above times the members.
- Latency adds the trip to the server: pick a server close to all players.
- The server never reads the relayed data, but anyone who knows a room's name (and its password) can join it. Use a password for private games.
  The room protocol is not encrypted.
- The public deployment at `pretend.napple.lat` runs this binary in a container with `NAPPLE_ROOM_LISTEN=0.0.0.0:31415` and
  `NAPPLE_ROOM_HTTP=0.0.0.0:8081` (served as `https://pretend.napple.lat/room/healthz` behind the proxy). See the napple-infra hand-off.

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

- Server: `cargo test -p napple-room` (14 tests: room life cycle, passwords, addresses, unicast / broadcast relay, spoofing, LDN advertise / scan /
  relay, limits, rate limit, keepalive, HTTP health and room list).
- Runtime: `make -C runtime test-net-room` runs a real server and two consoles (two processes) through the provider: nifm, UDP on the same port from
  both, broadcast, unicast, implicit bind, connected UDP, TCP with 100 kB both ways, LDN scan / join / nodes / advertise / leave / destroy, a member
  quitting.
- **Mario Kart 8 Deluxe** (update 3.0.5), two headless instances on one Mac in one room (mk8d-lan-session, 2026-11-02):
  - **LAN Play works.** One instance creates a room; the other sees it in its lobby list, joins it, and both reach the room's character select. Pia
    traffic flows both ways between 10.13.0.2 and 10.13.0.3.
  - **Wireless Play (local wireless) partly works.** The host creates its room and the other instance lists and joins it, but the joining instance
    then crashes inside the game (not diagnosed yet).
  - How to get there:
    - LAN Play: A on the title screen, then L+R+left stick together on the main menu, then "LAN Play".
    - Every player needs their own `--save-root`: that is what makes each instance a different console. Profile names are all "Napple" for now.
  - The fix that made it work was in bsd, not in rooms: the console's bsd service reports Linux errno numbers, and Napple's reported FreeBSD's, so the
    game's network library took "nothing to read yet" for a fatal error (error 2618-0006).

## Not done yet

- No UDP transport to the server (TCP only: a lost packet delays the ones behind it). The deployment opens 31415/udp in case one is added.
- No NAT traversal or direct peer-to-peer path: everything goes through the server.
- No room browser in Napple Studio: rooms are joined with environment variables.
- TCP between virtual addresses supports connect / accept / data / half-close. TCP to one's own virtual address, IP options and multicast are not
  modelled.
