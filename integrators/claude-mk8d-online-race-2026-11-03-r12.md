---
branch: claude/mk8d-online-race-2026-11-03-r12
orchestration: mk8d-online-race
base: ac6692c
head: see git log (hand-off messages name the head)
env: mac
date: 2026-11-03
areas: [crates/napple-online, runtime/net, docs/guides, compat]
status: ready-with-caveats
---

# claude/mk8d-online-race-2026-11-03-r12: MK8D online race: joiner fixed, two instances reach the race lobby

## Summary
- **Done (verified):** the joiner's 2618-0502 is fixed. Two MK8D instances on one Mac through a local napple-online + napple-room join one gathering,
  connect over Pia, and reach character select, kart selection and the course vote (screens in the notes folder `mk8d-online-race-r12/`).
- Cause 1: the joiner never heard of the host (no 3001 for existing participants). Cause 2: Pia identifies stations by IPv4 address only, and both
  instances shared the Mac's address, so the host refused the joiner (response result 1). Fix: distinct public addresses (room virtual addresses) end to end.
- **Update:** MatchmakeExtension 49 answered; with frame-timed vote presses both vote, the course is selected and the race countdown starts (verified, ~215 s run).
- **Not done:** (2) the 2618-0201 flake was not studied (never reproduced in about 25 pair runs today with the room; without a room runs were the same
  "2618-0502 / now refusal" outcome every time; the earlier flake could be load: three of my runs at 40 s ended before matchmaking because the game was slow
  under other sessions' load); (3) Friends list still empty; (4) Wireless Play station crash not diagnosed; the race itself not reached (120 s limit).

## Files
`crates/napple-online/src/server.rs` (existing-participant notifications, public address at Register), `natcheck.rs` (`reported_ip`, `public_ip`),
`runtime/src/net/net_online.c` (NAT check local IPv4 + virtual port), `runtime/src/net/net_host_sock.c` (`NAPPLE_NET_LOG=2`),
`runtime/tests/test_net_online_client.c`, `docs/guides/napple-online.md` (MK8D section), `compat/online.json`, change note. No shared file touched.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| `cargo test --release -p napple-online` | tested | 37 passed (new: joiner hears existing participants; reported_ip / public_ip) |
| `make -C runtime test-net-online` | tested | 55 checks / 0 failed; test_net_online_ssl 42 / 0 |
| Joiner reaches the connection request and Pia connects | verified | pair runs with `ROOM=1`: ~1700 692-byte datagrams each way between 10.13.0.2 and 10.13.0.3 |
| Both instances reach character, kart, course vote | verified | frame montages in the notes folder |
| Race started | verified | 225 s run: vote, "The course has been selected", Rosalina's Ice World, both players listed, countdown 3 (montage `race-starts-host-vote-to-countdown.png`) |
| Acceptance 5 (compat/online.json updated truthfully) | done | online_race_or_match partial |

## Findings
- `grep` in these sessions is a shell wrapper that silently drops matches on some patterns; use `/usr/bin/grep -a`.
- Pia (main+0x9ae624) looks a requesting station up by IPv4 only; a joiner behind the same address as the host is refused with result 1 (-> joiner error codes 0xc24).
- The main+0x99dd..0x99e5.. job functions the previous notes named are reached only by a joiner that passes main+0x9330e8 (the "is the host a participant" check).
- Without a room, two instances on one Mac cannot play together (one address). With a room each gets a virtual address; the NAT check reply's port must be the socket's virtual port.
- Same-machine clients are reported at their own address (NAT check field / Register URL); real clients (not loopback) are never affected.

## How to integrate
No ordering constraints. Hotspots: `server.rs` (online-identity adds files, not these hunks), `net_online.c` (client-core wires friend hooks in other places).
Build: `cargo build --release -p napple-online -p napple-room`; `make -C runtime`.

## How to test
1. `cargo test --release -p napple-online`; `make -C runtime test-net-online`.
2. Pair run (script `pair.sh` in the notes folder `mk8d-online-race-r12/`): `ROOM=1 lockf -k /Users/neto/NappleWork/.run.lock pair.sh <out> 118 5`; passing = `a`/`b` frames show character select -> kart -> course vote; failing = both back on "Worldwide".
3. Regression: without `NAPPLE_ROOM` the NAT check path is unchanged (field untouched, port untouched).

## Fix policy hints
Safe: the address helpers. Design decision: whether the public address for same-machine clients should become default in napple-online (today only through the client's own claim).

## Needs from other areas
- The 120 s run limit is too short for MK8D's online path to the race (boot ~27 s, menus ~20 s, matchmaking ~20 s, char/kart ~25 s, vote): ask the run-lock owner for 180 s for this title, or provide a quicker route (a save with the online menu pre-selected).
