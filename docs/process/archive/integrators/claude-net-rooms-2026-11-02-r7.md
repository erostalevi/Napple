---
branch: claude/net-rooms-2026-11-02-r7
orchestration: net-rooms
base: c44af844fafe86d87e8468fbc6437e1afaa7c55d
head: 0228a86 (the commit before this line was set)
env: mac
date: 2026-11-02
areas: [crates/napple-room, runtime/net]
status: integrated
---

# claude/net-rooms-2026-11-02-r7: LDN and LAN play between Napple instances over the internet (napple-room server + runtime client)

## Summary
- **Room server** `crates/napple-room/` (Rust, std only): rooms with an optional password, member list, virtual addresses from a configurable subnet,
  keepalive, rate limits, size limits, relay of virtual-LAN packets (unicast and broadcast fan-out, anti-spoofing) and of LDN frames, plus LDN
  advertise / scan. `napple-room list HOST` lists rooms. Own protocol (`proto.rs`, version 1).
- **Runtime client** `runtime/src/net/net_room*.{c,h}`, inside the r6 host provider, active only with `NAPPLE_ROOM`:
  - Virtual LAN (nifm reports a wired 10.13.x.y/16 interface; guest UDP/TCP to virtual addresses and broadcasts go through the room).
  - LDN through the room (advertise, scan, JOIN / ACCEPT / NODES / BYE as room packets, node addresses are virtual addresses).
  - Several instances on one Mac all bind 49152.
- Small hooks in `net_host_sock.c`, `net_host_ldn.c` and `net_host.c`. Docs: `docs/guides/online-rooms.md` (use, deployment, design) and env vars.
- r6 (net-host-provider) merged twice, the second time at 80a8880. My socket trace was dropped in favour of r6's `NAPPLE_NET_LOG`.
- **Not done (acceptance 3):** two MK8D instances do not reach a LAN Play lobby that shows each other. The network part works: each instance receives
  the other's LAN announcement over the room from the other's virtual address. MK8D then aborts with error 2618-0006 about 2 s into the session search.
  It does that with a single instance on the real LAN without rooms too, and in Wireless Play (LDN), so the cause is outside the relay. Not diagnosed.
  Two more blockers outside my paths: nifm GetCurrentIpConfigInfo is a stub (LAN Play cannot start without it; patch below), and GetPseudoDeviceId
  is the same for every instance.
- Studio UI: out of scope; what it would need is under "Needs from other areas".

## Files
`git diff --stat c44af84..HEAD` (excluding the r6 merge):
- New: `crates/napple-room/{Cargo.toml, src/lib.rs, src/proto.rs, src/server.rs, src/client.rs, src/main.rs, tests/room.rs}`,
  `runtime/src/net/net_room.h`, `net_room.c`, `net_room_vlan.c`, `net_room_ldn.c`, `runtime/tests/test_net_room.c`, `docs/guides/online-rooms.md`,
  `docs/archive/changes/2026-11-02-claude-net-rooms-2026-11-02-r7.md`, this file.
- Changed (hooks, additive): `runtime/src/net/net_host_sock.c`, `runtime/src/net/net_host_ldn.c`, `runtime/src/net/net_host.c`.
- Shared, append-only: `Cargo.toml` (workspace member), `Cargo.lock` (the new package), `runtime/Makefile` (test rules at the end),
  `docs/reference/environment-variables.md` (new section after the host-provider table), `docs/guides/README.md` (one row).
- Not touched: `offline*.c`, `account*.c`, `ldn.c`, `ldn_core.c`, `applet*.c`, `gfx/`, `crates/napple-online/`, `orchestrations/`.

## Built and tested here
| Item | Status | Evidence (command + result) |
|---|---|---|
| Acceptance 1: `cargo test -p napple-room` (lifecycle, password, broadcast fan-out, LDN frame relay, limits) | tested | `cargo test -p napple-room`: 4 unit + 9 integration tests, ok; 3 repeated runs clean |
| Acceptance 2: two runtime instances on this Mac, each on a local napple-room: LDN scan sees the other's network, join, data | tested (two processes, real server, real provider) | `make -C runtime test-net-room`: `test_net_room: 39 checks, 0 failed`, `(console B): 22 checks, 0 failed`; 5 repeated runs clean. Covers UDP same-port binds, broadcast, unicast, implicit bind, connected UDP, TCP 100 kB both ways + EOF + refused, LDN scan / join / nodes / advertise / leave / destroy / rejoin, member quitting |
| Acceptance 3: two MK8D instances meet in the same LAN Play lobby | **not reached** (network part verified) | Pair run below: each instance gets the other's 873-byte announcement on UDP 30000 from 10.13.0.x via the server. Both then show error 2618-0006 (error applet 0xe, then the r6-noted PopOutData abort). Same error alone on the real LAN without rooms, and in Wireless Play |
| Acceptance 4: without `NAPPLE_ROOM`, behaviour as r6 | tested + verified | r6 tests unchanged and green (`test_net_host_ldn`, `_sock`, `_tls`); boots below; with no room configured every hook returns "not handled" |
| Acceptance 5: `make test test-services test-ipc-reference` | tested | exit 0; 80 test binaries; `ipc_reference` 111 fixtures, missing 0 (after the second r6 merge) |
| Acceptance 5: four-title boots, no regression | verified | SMO, LAS, MK8D, ACNH, 45 s each, offline and `NAPPLE_NET=host`: rc 0, last stage notify-running, `unsupported_svcs []` |
| Deployment contract (orchestrator / napple-infra request) | verified on macOS | `cargo build --release --locked -p napple-room --bin napple-room` ok, links only libSystem; `NAPPLE_ROOM_LISTEN=127.0.0.1:31415 NAPPLE_ROOM_HTTP=127.0.0.1:8081 NAPPLE_ROOM_DATA=/nonexistent napple-room`: `curl /healthz` = `ok`, `curl /rooms` = `{"version":1,"clients":0,"rooms":[]}`, `napple-room health` rc 0, `health 127.0.0.1:1` rc 1, logs on stdout, `kill -TERM` = exit 0, bad env value = exit 2 with usage |
| HTTP `/healthz`, `/rooms` | tested | `cargo test -p napple-room` `http_health_and_room_list` (14 tests in all) |
| `NAPPLE_ROOM=default[/<room>]` = pretend.napple.lat:31415 | tested (parser) | `test_net_room` checks `nr_parse_room` forms (45 + 28 checks); never connected to the real server |
| Server on Linux | unbuilt | no Linux target or container runtime on this Mac. The crate is std-only and has no `cfg(target_os)`; the only FFI is libc `signal` / `_exit` (present on glibc and musl). Build it in the napple-infra image first |
| Server CLI `serve` / `list` | verified | ran `napple-room serve --listen 127.0.0.1:31499` for the title runs; server log shows joins and leaves |
| Room reconnect after a server loss | untested | code path only (`net_room.c` worker: redial every 3 s with the previous address) |

Pair run (MK8D update v1376256, both instances under one run lock, private save copies, 60 s):
`NAPPLE_NET=host NAPPLE_ROOM=127.0.0.1:31499/mk8d NAPPLE_ROOM_NICK=alice|bob NAPPLE_INPUT_SCRIPT="14:A,18:L+R+STICK_L:0.6,20:DOWN,21:DOWN,22:DOWN,23.5:A"`,
plus the local `GetCurrentIpConfigInfo` patch below (needed to get past "Connecting..."). Log excerpt (instance at 10.13.0.2):
```
net: room: joined 'mk8d' on 127.0.0.1:31499 as 10.13.0.2 (member 1, 1 in the room, virtual LAN on)
net: room: 'alice' joined as 10.13.0.3
net: sock 0 (udp) bind 0.0.0.0:49152 -> ok          (sock 1: 40000, sock 2: 30000; SO_BROADCAST + SO_REUSEADDR)
net: sock 2 recvfrom 10.13.0.2:30000 (873 bytes)    (its own broadcast, looped back)
net: sock 2 recvfrom 10.13.0.3:30000 (873 bytes)    (the other instance's, through the server)
applet: error applet: type 0, jump-to-settings requested   (args: 00010000 01000100 3a0a0000 06000000 ...  = 2618-0006)
```

## Unbuilt / untested list (the integrator checks these first)
Everything was compiled and run on this Mac. Never exercised by a test:
- The `net_room.c` reconnect path (server restart while joined), commit 2fec3b3.
- A real server over the internet (all runs were 127.0.0.1); the IPv6 `[addr]:port` form of `NAPPLE_ROOM`.
- `NAPPLE_VLAN=off` and `NAPPLE_VLAN=<address>` from the environment (`want_vip` is tested at the server level only).

## Findings
- **MK8D LAN Play** (title evidence; screenshots and logs in the shared notes folder `net-rooms-r7/`):
  - Entering it: A on the title screen, then L+R+STICK_L pressed together on the main menu (a press during the title screen is unreliable). It needs
    nifm `GetCurrentIpConfigInfo` (cmd 15, a stub answering 2110-1111 → "Connecting..." then back to the menu).
  - With that answered, MK8D opens three UDP sockets with SO_BROADCAST + SO_REUSEADDR, bound 0.0.0.0:49152 (Pia), 40000 and 30000, and broadcasts an
    873-byte announcement to the subnet broadcast on 30000. 2 s later it sends a 72-byte broadcast on 49152 and, about 30 ms later, shows error
    2618-0006.
  - The same happens in Wireless Play (LDN) about 60 ms after CreateNetwork, with r6's advertise fix, both through r6's LAN transport and through the room.
- **What 2618-0006 is not:** tried and ruled out:
  - the GetCurrentIpConfigInfo layout (the game computes the right broadcast address from it);
  - dropping self-addressed datagrams;
  - a failing socket call (none fails; `NAPPLE_NET_LOG`);
  - a failing svc (`NAPPLE_SVC_TRACE` over the window: no errors, no thread creation);
  - a failing or stubbed IPC (`NAPPLE_TRACE_IPC=all`: none in the window);
  - a non-zero / unique GetPseudoDeviceId.

  The decision is taken in game code: a generic task dispatcher at main+0x86e5b8 shows an `ErrorResultVariant` stored by an earlier step. Next step:
  find what stores the error, for example a `NAPPLE_WATCH` on that object once its address is known (heap), or the Pia session-start code from the
  caller chain.
- r6's hypothesis for the LDN case was that the game wants 169.254/16 node addresses. The server takes `--subnet 169.254.0.0/16` to test it:
  tried here: with 169.254.0.2 the Wireless Play host still shows 2618-0006 60 ms after CreateNetwork, so the node address range is not the cause.
- **Found outside my area (not fixed):**
  1. `nifm IGeneralService::GetCurrentIpConfigInfo` (offline.c, stub) blocks every LAN-play mode. Patch: `nifm-GetCurrentIpConfigInfo.patch` in the
     shared notes folder; it is the 0x16-byte IpConfigInfo from the provider's net_info, addresses as in_addr, 2110-1111 when not connected.
  2. `IApplicationFunctions::GetPseudoDeviceId` (applet.c) is the constant "Napple" plus zeros for every instance. Two Napple players present the same
     device id to each other. It should be unique per installation (a random UUID kept in the save root, for example).
  3. `ipc.c`: domain object ids are never reused (`DOMAIN_MAX` 512). The MK8D Wireless Play lobby makes a new `acc:u0` IProfile every 200 ms while it
     scans, so the runtime stops with "IPC domain full" after about 100 s in that lobby. Any object made in a loop hits this.
  4. `nifm GetCurrentIpAddress` returns the address in host order through `IPC_OUT` (bytes reversed against in_addr); r6's test pins that order.
     Not used by MK8D; worth checking against a title that uses it.
  5. Error applet: after 2618-0006 the game pops the applet's output, gets 0x680 and Breaks (already in r6's integrator file).
- Decisions:
  - Room transport is one TCP connection per client (one port, NAT-friendly, no client port forwarding), no UDP yet.
  - Server std-only, threads (one reader per client plus a writer per client with a bounded queue). A slow member loses relayed packets, never stalls
    the room; it is disconnected only when even a control frame does not fit.
  - The virtual LAN keeps guest sockets as real host sockets (loopback injection and splices) rather than emulating sockets in user space, so r6's
    blocking, timeouts, poll and close-from-another-thread logic applies unchanged.
  - A virtual-LAN socket is bound to an ephemeral host port: several instances on one Mac, and no conflict with the real LAN.
  - A broadcast also reaches the sender's own sockets, as BSD does.
  - LDN in a room needs the virtual LAN (node addresses must be virtual); with `NAPPLE_VLAN=off` hosting fails cleanly.
  - nifm in a room reports "connected" only while joined.
  - The room client is process-wide (one console per process). The r6 provider's per-instance LDN state is bypassed while a room is configured.
  - `test_net_room` forks B before A starts its room client (a fork after would inherit A's connection).

## Deployment contract (napple-infra, pretend.napple.lat)
This matches the contract in the shared notes file `napple-infra-server-contract.md`, with no renames:
- Binary `napple-room`.
- `NAPPLE_ROOM_LISTEN` (TCP 31415, `proto::DEFAULT_PORT`).
- `NAPPLE_ROOM_HTTP` (plain HTTP, `GET /healthz` = `200 ok`, `GET /rooms` = JSON).
- `NAPPLE_ROOM_DATA` is accepted and unused: rooms are memory-only, so the server needs no writable volume.
- Extra knobs: `NAPPLE_ROOM_SUBNET`, `_MAX_MEMBERS`, `_MAX_ROOMS`, `_MAX_CLIENTS`, `_RATE_KBPS`, `_RATE_PPS`, `_KEEPALIVE`, `_QUIET`. Each has a flag of
  the same name that wins.
- **UDP is not used**: 31415/udp can stay closed until a UDP fast path exists.
- Container healthcheck without curl: `napple-room health` (exit 0 / 1). Logs go to stdout. SIGTERM / SIGINT exit 0. Runs fine as uid 10001 on a
  read-only root filesystem (it writes nothing).
- Client: `NAPPLE_ROOM=default` (or `default/<room>`) means `pretend.napple.lat:31415`, still opt-in. `NAPPLE_ROOM=pretend.napple.lat:31415` works
  too.

## How to integrate
- Merge order: after net-host-provider (r6), which is merged in at 80a8880. If r6 gets more commits, merge them and re-run the tests.
  napple-online (r7) also builds on r6. Overlap: both add `#include`s and hooks in `runtime/src/net/net_host.c` / `net_host_sock.c` (mine are marked
  `/* rooms: ... */`), and both append to `Cargo.toml` members and `docs/reference/environment-variables.md`. Keep both sides.
- Conflict hotspots: `Cargo.toml` members line (keep both crates), `Cargo.lock` (regenerate with `cargo build`), `runtime/Makefile` tail (both
  appends), `net_host.c` provider table (I replaced `.net_info` / `.net_connect` with `nh_net_info_any` / `nh_net_connect_any`; if napple-online wraps
  the table, keep the wrappers).
- Build and test, in order (expected result):
  1. `cargo test -p napple-room`: 13 tests ok.
  2. `make -C runtime test-net-room`: `test_net_room: 39 checks, 0 failed` (builds the server with cargo; the Makefile looks for cargo on PATH, then in
     `~/.cargo/bin`, then `~/.rustup/toolchains/*/bin`).
  3. `make -C runtime test test-services test-ipc-reference`: exit 0.
- No Linux-only calls. The client uses `arc4random_buf` and BSD `sin_len` like r6's provider; the server is portable std Rust (macOS / Linux).

## How to test
1. Unit/regression: steps 1-3 above. Failing looks like `FAIL tests/test_net_room.c:<line>` or a non-zero `console B` count. A hang ends at 120 s
   (alarm).
2. Headless pair (needs the GetCurrentIpConfigInfo patch to go past "Connecting..."): start `target/debug/napple-room serve --listen 127.0.0.1:31499`,
   then two runtimes, as above, with private `--save-root` copies, under one run lock. Pass today means: in each log `room: joined`, the other's
   `room: '<nick>' joined`, and with `NAPPLE_NET_LOG=1` a `recv(...)` from the other's 10.13.0.x on port 30000. The lobby is blocked by 2618-0006 (see
   Findings).
3. Regression guard: without `NAPPLE_ROOM` nothing changes. r6's `test_net_host_*` must stay green, and the four titles must boot as before.
4. Human: two Macs (or one Mac and a friend) with a reachable server: `NAPPLE_NET=host NAPPLE_ROOM=<server>/<room>` on both, MK8D Wireless Play /
   LAN Play. Expected today: the same error 2618-0006 until the gap above is fixed.

## Fix policy hints
- Safe to fix directly: typos, includes, Makefile cargo lookup, test timing (the test has generous waits; the `usleep` loops can be raised).
- Needs a design decision:
  - A UDP data channel to the server (lower latency on lossy links). Option: keep TCP for control, add an authenticated UDP socket per member.
  - Whether rooms should start without `NAPPLE_NET=host` (today `NAPPLE_ROOM` does nothing unless the host provider is selected).

## Needs from other areas
- offline.c owner (nifm): implement `GetCurrentIpConfigInfo` (patch in the shared notes folder `net-rooms-r7/nifm-GetCurrentIpConfigInfo.patch`).
  With a room, the provider's net_info already reports the virtual interface, so the patch needs nothing room-specific.
- applet.c owner: a per-installation `GetPseudoDeviceId`.
- ipc.c owner: reuse closed domain object ids (MK8D Wireless lobby dies after about 100 s).
- applet owner: the error applet output data (the Break after any error screen; r6 already reported it).
- Whoever takes the 2618-0006 investigation (see Findings): MK8D LAN / Wireless sessions abort about 30-60 ms after Pia starts.
- Studio: a "Play online with friends" panel. It needs server, room, password and nickname fields (written as `NAPPLE_NET=host`, `NAPPLE_ROOM`,
  `NAPPLE_ROOM_PASSWORD`, `NAPPLE_ROOM_NICK` in the launch environment), an optional `napple-room list` call to show rooms, and the joined /
  member-left lines from the log as a member list.

## Integration result
- Date 2026-11-02, merged into main as faba99f (branch head 93da095 as handed off; first item of the r7-r9 integration run). No conflicts. Outcome: **Clean**, plus one doc
  commit `integrate: net-rooms change note in the r7 change archive table` (`tools/check_docs.py` reported the change note as an orphan before, OK after).
- No native ports: the client is BSD sockets + pthreads + `arc4random_buf` (already macOS-native), the server is portable std Rust; C left as written.
- `cargo test -p napple-room`: 4 unit + 10 integration tests ok (the file says 9 + 4 = 13; the HTTP test makes it 14).
- `make -C runtime GFX=napple test-net-room`: `test_net_room: 45 checks, 0 failed`, `(console B): 28 checks, 0 failed` (the file's 39 / 22 predate the
  deployment-contract commit's parser checks).
- `make -C runtime GFX=napple test test-services test-ipc-reference`: rc 0, ipc_reference 111 fixtures, app_reachable 1325, missing 0, errors 0. Build warnings
  only the three pre-existing ones (applet_ae.c cast, bcat.c unused, stb_vorbis).
- Two-instance LAN relay smoke (How to test 2), one job under the run lock, 45 s, private save copies, local `target/debug/napple-room serve` on
  127.0.0.1:31499, `NAPPLE_NET=host NAPPLE_ROOM=127.0.0.1:31499/mk8d NAPPLE_NET_LOG=1`, the file's input script, with the shared-notes
  `nifm-GetCurrentIpConfigInfo.patch` applied locally and reverted afterwards (not committed: nifm is mk8d-lan-session's): both instances `room: joined`
  (10.13.0.2 / 10.13.0.3), each logs the other's join, each `recv(2, …) = 873 … from` the other's 10.13.0.x:30000 at 24.7 s; both then show error
  2618-0006 at 26.7 s exactly as the file reports. Server log: two joins, two leaves. Both rc 0.
- Four-title 45 s headless boots (run lock, private save copies, shared title data read-only), without and with `NAPPLE_NET=host`: SMO, LAS, MK8D
  (v1376256), ACNH (3.0.3) rc 0, last stage notify-running, `missing_ipc []`, `unsupported_svcs []`, stubbed only `nvdrv:13`; no `room:` line without
  `NAPPLE_ROOM`.
- Corrections to the file: `NAPPLE_NET_TRACE` in the shared-notes `run_pair.sh` is `NAPPLE_NET_LOG` now (the file already says so); test counts above.
- Still untested (as declared): reconnect after a server loss, a server over the internet, IPv6 `NAPPLE_ROOM`, `NAPPLE_VLAN=off|<address>`, Linux server build.
