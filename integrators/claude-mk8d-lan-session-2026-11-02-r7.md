---
branch: claude/mk8d-lan-session-2026-11-02-r7
orchestration: mk8d-lan-session
base: 93da095 (claude/net-rooms-2026-11-02-r7 hand-off head)
head: f74f563 (the commit before this line was set)
env: mac
date: 2026-11-02
areas: [runtime/services (bsd, nifm, am, friend, ldn), runtime/net]
status: ready-with-caveats
---

# claude/mk8d-lan-session-2026-11-02-r7: two MK8D instances meet in LAN Play (error 2618-0006 fixed)

## Summary
- **Root cause of 2618-0006, fixed:** the bsd service answered with FreeBSD errno numbers, but the console's bsd answers with Linux's.
  - MK8D's Pia compares `GetLastErrno()` with 11 (EAGAIN). Napple's 35 made Pia's receive thread give up at its first empty read, and the session
    ended with 2618-0006.
  - The fix maps errnos at the bsd service boundary; it is generic, for every title.
- Also fixed:
  - nifm `GetCurrentIpConfigInfo` (LAN Play could not start).
  - A unique, stable `GetPseudoDeviceId` per installation + title + user.
  - friend `GetPlayHistoryRegistrationKey`: the Wireless host destroyed its room when it failed.
  - ldn `GetSecurityParameter`: 0x20 bytes with the network's SessionId, consistent with NetworkInfo.
- **Verified:** two MK8D instances on this Mac through a local napple-room in LAN Play. One creates a room, the other lists it, joins, and both reach
  the room's character select, with Pia traffic both ways.
- **Wireless Play (local wireless) through the room:** host and listing work and the station joins, then the station crashes inside the game.
  Not diagnosed; it is the remaining part of acceptance 2.

## Files
- New: `runtime/src/services/install_id_core.{c,h}`, `runtime/tests/test_svc_lan_play.c`,
  `docs/archive/changes/2026-11-02-claude-mk8d-lan-session-2026-11-02-r7.md`, this file.
- Changed:
  - `runtime/src/services/bsd_core.{c,h}` (errno map).
  - `runtime/src/services/offline.c`: bsd replies, nifm 15, friend 10700.
  - `runtime/src/services/applet.c` (GetPseudoDeviceId row).
  - `runtime/src/services/ldn.c`, `ldn_core.{c,h}`: GetSecurityParameter, NetworkInfo SessionId.
  - `runtime/src/net/net_room_ldn.c`.
  - Tests: `test_svc_offline.c`, `test_svc_online.c`, `test_svc_ldn.c`, `test_svc_nifm_audit.c`, `test_net_host_sock.c`. Expectations on bsd IPC
    replies now use `bsd_errno_to_guest(HZ_E...)`, and the "15 is a stub" pin is now "15 is real".
- Docs: `docs/guides/online-rooms.md` (Status section).
- Service rows the trace pointed at (owned by this item per the assignment): bsd reply errno (all calls), nifm 15, am 50 GetPseudoDeviceId, friend
  10700, ldn 4 GetSecurityParameter.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| Acceptance 1: root cause identified with IPC / socket trace, fixed generically | verified | `NAPPLE_NET_LOG=1` with a thread id: Pia's receiver (thread 0x80) read once (EAGAIN) and never again. The Break thread dump shows it in `nn::os::WaitEvent` from main+0x9b5bd4 (the state-1/3 branch of Pia's worker loop main+0x9b5b6c). Pia compares errno with 11 / 100 / 9 (main+0x9d0a60, ...). The play report shows `ErrorCode 2618-6`, page `Local_LobbySelect`. After the fix: Pia's socket is read 2,334 times in 15 s, and no error |
| Acceptance 2: two MK8D instances in the same LAN Play lobby via a local napple-room | verified | final run on the committed code (one job under the run lock, 75 s, private save roots, room 127.0.0.1:31499/mk8d). B's lobby lists A's room "Napple 1/12" (frame ~2100); B selects it, joins, and both reach the room's character select (frames 2400-3300). Unicast Pia traffic: A to 10.13.0.2:49152 2,276 packets, B to 10.13.0.3:49152 2,217. No error applet, both rc 0. Screenshot montages and logs: shared notes folder `mk8d-lan-session-r7/` |
| Acceptance 2: Wireless Play via the room | partial | A hosts (no error since the friends key fix), and B's scan lists it (`room ldn: scan ... finds 1 network(s)`). B joins (`joined ... as node 1`), then crashes: SIGSEGV, a null vtable, at main+0x9a3c84 or main+0x9c1810 depending on the run. Not diagnosed |
| Single instance LAN Play on the real LAN (no rooms) | verified | the LAN lobby stays up 40 s, rc 0, no error applet |
| Acceptance 3: `make -C runtime test test-services test-ipc-reference` | tested | exit 0, 80 test binaries, `ipc_reference` 111 fixtures / missing 0; `test_svc_lan_play` 21/0; `test_net_room` 39 + 22 checks |
| Acceptance 3: four-title boots | verified | SMO, LAS, MK8D, ACNH, 45 s each, offline and `NAPPLE_NET=host`: rc 0, last stage notify-running, no unsupported svc (after the errno change) |

## Unbuilt / untested list
Everything was built and run on this Mac. Not exercised:
- A friend `AddPlayHistory` round trip (still a stub that answers success).
- `install_id_get` when the save root cannot be written: falls back to a per-process random id; the fallback path itself is not tested.

## Findings
- **errno numbering is project-wide.**
  - `hz_result.h` says the guest sees FreeBSD numbers. That holds for the runtime's internal numbering, not for what bsd replies.
  - Any title that compares errno (EAGAIN / EINPROGRESS for non-blocking sockets) was affected; NEX online code very likely too (napple-online r7
    should re-test with this).
  - Other services that return an errno-like value outside bsd (sfdnsres h_errno / EAI, ssl) were not changed.
- MK8D LAN Play needs:
  - nifm GetCurrentIpConfigInfo;
  - two consoles with different pseudo device ids. Each instance needs its own `--save-root`; the installation id lives there.
- MK8D Wireless Play (host) calls friend `GetPlayHistoryRegistrationKey` right after hosting and destroys the room when it fails.
- MK8D enters LAN mode with L+R+left stick pressed together **on the main menu** (after leaving the title screen); a press during the title screen is
  unreliable.
- Found, not fixed (other areas):
  1. `g_user_id` (account.c) is one constant ("NappleUser...01") for every installation, so both players are "Napple" with the same user id. LAN Play
     works anyway; a per-installation user id belongs to napple-account.
  2. `ipc.c` never reuses domain object ids (`DOMAIN_MAX` 512). MK8D's Wireless lobby makes an `acc` IProfile every 200 ms while it scans, so the
     scanning instance stops with "IPC domain full" after about 25-100 s in that lobby. It hit B in one run here.
  3. The station crash in Wireless Play (above): Pia objects with a null vtable right after Connect. It is a race or a missing service behaviour.
     Next steps: run with `NAPPLE_TRACE_IPC=all` on both ends, and compare the station's GetNetworkInfo node list and state-change events with a
     console's.
- Tooling note: lldb cannot drive the runtime. Attaching breaks its trapped-instruction / SVC handling, and the guest dies with SIGILL. Static
  analysis (llvm-objdump on `modules/*.patched.bin` via the nifm-netconnect `mkelf.py`) plus thread dumps and `NAPPLE_NET_LOG` was enough here.

## How to integrate
- Lands after net-rooms (r7), which is the base.
- Conflict hotspots:
  - `offline.c`: nifm rows, the friend 10700 row, `bsd_reply`. napple-online edits nsd / acc rows in the same file; the hunks are separate.
  - `applet.c`: one function.
  - Tests: if another branch adds bsd IPC checks with `HZ_E...` expectations, they need `bsd_errno_to_guest()`.
- Build and test:
  1. `make -C runtime test test-services test-ipc-reference`: exit 0.
  2. `make -C runtime build/tests/test_svc_lan_play && ./runtime/build/tests/test_svc_lan_play`: 21 checks, 0 failed.

## How to test
1. Unit tests: as above.
2. Two-instance LAN Play (one job under the run lock, ≤120 s):
   - Start `target/debug/napple-room serve --listen 127.0.0.1:31499`.
   - Run two runtimes with `NAPPLE_NET=host NAPPLE_ROOM=127.0.0.1:31499/mk8d NAPPLE_NET_LOG=1` and **separate** `--save-root` copies. Scripts:
     - A: `14:A,18:L+R+STICK_L:0.6,20:DOWN,21:DOWN,22:DOWN,23.5:A,28:A`
     - B: the same up to 23.5, then `38:DOWN,39.5:A`.
   - Pass: no `applet: error`, both rc 0. B's screenshots show A's room in the lobby (~frame 2200) and then the character select. Both logs show
     `send(0, ..., to 10.13.0.x:49152)` unicasts.
   - The runner used here is `run_pair.sh` in the shared notes folder `net-rooms-r7/`.
3. Regression: four-title boots offline and with `NAPPLE_NET=host`.

## Fix policy hints
- Safe: errno table entries. Add a FreeBSD→Linux pair if a title shows an unmapped errno.
- Needs a decision:
  - Whether the provider ABI should move to the guest numbering instead of mapping at the boundary. Mapping keeps the providers and their tests
    unchanged; recommended to keep.

## Needs from other areas
- account (napple-account): a per-installation user id and nickname, so players are told apart in lobbies ("Napple" for everyone today).
- ipc.c owner: reuse closed domain object ids.
- Whoever takes Wireless Play next: the station crash above.
