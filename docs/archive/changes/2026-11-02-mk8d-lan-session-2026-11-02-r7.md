# mk8d-lan-session (2026-11-02-r7): two MK8D instances meet in LAN Play (error 2618-0006 fixed)

Branch `claude/mk8d-lan-session-2026-11-02-r7`, Mac session, based on net-rooms (r7). Integrator file: `integrators/claude-mk8d-lan-session-2026-11-02-r7.md`.

## Root cause of 2618-0006
- The console's bsd sysmodule answers with Linux errno numbers (EAGAIN 11, EINPROGRESS 115, ECONNREFUSED 111, ENETDOWN 100; yuzu and Ryujinx do the
  same). The SDK's `nn::socket::detail::RecvFrom` stores the service's errno unchanged.
- MK8D's Pia checks `GetLastErrno()` after a receive: 11 means "would block", 100 "network down", 9 "bad descriptor" (main+0x9d0a60 and siblings).
- Napple answered FreeBSD's numbers (EAGAIN 35). Pia's receive thread (main+0x9b5b6c) took its first empty non-blocking read as a failure, entered
  its error state and parked in WaitEvent.
- The next transport check failed, and the game showed 2618-0006 (play report: `ErrorCode 2618-6`, page `Local_LobbySelect`). In LAN Play that was
  2 s into the session search; in Wireless Play, 60 ms after CreateNetwork.
- How it was found: the socket log showed thread 0x80 read Pia's socket only once. The thread dump showed it parked in `nn::os::WaitEvent` inside
  that loop. The loop's state table decodes to "error state". Pia's errno comparisons settled it.

## What changed
- `bsd_core.[ch]`: `bsd_errno_to_guest()` maps FreeBSD to Linux numbers. `offline.c` `bsd_reply` / `bsd_reply3` and the `SO_ERROR` option value use
  it; the runtime and providers keep FreeBSD's numbers inside.
- `offline.c` nifm 15 `GetCurrentIpConfigInfo`: the provider's interface as an IpConfigInfo (0x16 bytes, in_addr fields). LAN Play stopped at
  "Connecting..." without it.
- `install_id_core.[ch]` (new) + `applet.c` `GetPseudoDeviceId`: a version-4 UUID per installation (16 random bytes in
  `<save root>/system/installation-id`), application and user, stable across runs. It was one constant for every instance.
- `offline.c` friend 10700 `GetPlayHistoryRegistrationKey`: a 0x40-byte key (type 0x101, key index, local flag, user id, hash keyed with the
  installation id). MK8D destroyed its hosted Wireless room when this failed.
- `ldn.c` / `ldn_core.[ch]`: `GetSecurityParameter` returns 0x20 bytes (data + SessionId), and NetworkInfo's NetworkId carries the same SessionId
  (derived from the network's random bssid / security data, so every node agrees).
- `net_room_ldn.c`: the joining station's ACCEPT already carries the node list, so it gets no second NODES. Scan result changes are logged.
- Tests: `test_svc_lan_play` (new, 21 checks). The IPC-reply expectations in `test_svc_offline`, `test_svc_online`, `test_svc_ldn`,
  `test_svc_nifm_audit` and `test_net_host_sock` now use the guest numbering.

## Status
- Verified: two MK8D (update v1376256) instances, one local napple-room, LAN Play.
  - A creates a room; B's lobby lists it ("Napple 1/12").
  - B joins, and both reach the room's character select.
  - Pia unicast flows both ways on 49152, thousands of packets each way.
  - Single instance on the real LAN without rooms: the LAN lobby stays up (rc 0, no error).
- Wireless Play (local wireless) through the room: A hosts (no error), and B's scan lists the network and joins as node 1. B then crashes in the game:
  a list entry with a null vtable, at main+0x9a3c84 or main+0x9c1810 depending on the run. Not diagnosed.
- `make -C runtime test test-services test-ipc-reference` green; four-title boots: see the integrator file.
