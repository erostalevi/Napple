# 2026-11-03 mk8d-online-race (r12): two MK8D instances reach the race lobby

Branch `claude/mk8d-online-race-2026-11-03-r12`.

- **Joiner failure 2618-0502 fixed (two causes).** Found by breakpoints on the Pia connect job (`NAPPLE_DEBUG_LOG`): (1) the joiner's connect step refuses a
  station it has not heard of, so napple-online now sends a joiner a 3001 "participant joined" for every participant already in a verbose gathering, before
  its own; (2) the host's connection-request handler looks a station up by IPv4 address only, so two consoles at one address are one station and the host
  refuses the joiner. A client on the server's machine now has its own public address: runtime (`net_online.c`) puts the console address in the NAT check's
  "local IPv4" field and the socket's virtual port in the reply; napple-online (`natcheck.rs`, `server.rs`) uses both for loopback clients and the Register
  URL's own address. Needs a room (each instance a virtual address); automatic while a room's virtual LAN is up.
- **Result (verified):** local napple-online + napple-room, two instances, one job under the run lock: both join, exchange Pia traffic (thousands of
  692 / 1044-byte datagrams each way) and reach the character, kart and course vote screens. The race itself was not reached inside the 120 s run limit.
- `NAPPLE_NET_LOG=2` dumps the first 200 bytes of every datagram (debug aid, `net_host_sock.c`).
- Tests: `cargo test -p napple-online` 37; `make -C runtime test-net-online` 55 / 42 (new: the NAT check local-IPv4 fill).
- Files: `crates/napple-online/src/{server,natcheck}.rs`, `runtime/src/net/{net_online,net_host_sock}.c`, `runtime/tests/test_net_online_client.c`,
  `docs/guides/napple-online.md`, `compat/online.json`.
