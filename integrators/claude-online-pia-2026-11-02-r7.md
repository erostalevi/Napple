---
branch: claude/online-pia-2026-11-02-r7
orchestration: online-pia (follow-up assignment by message from the orchestrator; no orchestrations/ file)
base: 193474f (claude/napple-online-2026-11-02-r7 head; the integration froze napple-online at f7c5b68, so this branch also carries 7dde717..193474f: the ssl IoMode / ResultWouldBlock change with its test, and docs)
head: 5da70dd
env: mac
date: 2026-11-02
areas: [crates/napple-online, runtime/net (net_online*), runtime/services (offline.c ssl functions, via 7dde717)]
status: ready-with-caveats
---

# claude/online-pia-2026-11-02-r7: past SessionKeepFailed to Pia's NAT check; two MK8D instances in one gathering

## Update (after merging mk8d-lan-session e6a4dd9)
- **Merged** `claude/mk8d-lan-session-2026-11-02-r7` (bsd errno numbering, nifm GetCurrentIpConfigInfo, GetPseudoDeviceId per instance). Conflicts:
  `Cargo.toml` members and `runtime/Makefile` test blocks, both additive, both kept.
- **The host no longer fails.** 2618-0006 is gone for the host (mk8d-lan-session's bsd errno fix). Instance A creates the gathering, runs its NAT
  check, publishes its station (ReplaceURL), calls MatchmakeExtension 34 UpdateProgressScore and waits on the kart screen. No error through
  95 s (screenshots).
- **Server fixes from this round (a045e28):**
  - ReplaceURL never matched the stored URL: the client names the *public* URL we gave it, not the private one it registered. So GetSessionURLs
    handed the joiner the host's WebSocket URL (port 1). Now Register keeps the private URLs plus the public one, and ReplaceURL replaces the
    public entry (address+port, else port, else first).
  - `NAPPLE_ONLINE_NATCHECK_BASE` shifts every NAT check port on both server and runtime. This was needed to test beside the integration
    session's server, which holds 8737 and UDP 10025….
- **The joiner still fails with 2618-0502.** B gets the host's private and Pia station URLs (`address=192.168.5.229;port=<A's Pia
  port>;natf=1;natm=1`), runs NatDetectionJob (three NAT-traversal dummy messages to nncs1:33335), polls ~35 ms, then calls MatchMakingExt 1
  EndParticipation and errors. It never sends a packet to A.
- What I tried for the joiner:
  - echoing the dummies back unchanged: they arrive, no change; dropped again;
  - B started 0.6 s or 5 s after A: 0.6 s is a race (the host's ReplaceURL comes later), 5 s is clean, same 2618-0502;
  - nifm GetCurrentIpAddress in network order (local test): no change for this step. It is still host order in `nifm_ip`, unlike
    GetCurrentIpConfigInfo, so the private URL carries 229.5.168.192. Listed under Needs, not committed (not my row).
- Next suspects for 2618-0502:
  - the Switch station URL keys the server may be expected to fill (`Ntrpa`/`Ntrpp` NAT-traversal public address and port, `R`/`Rsa`/`Rsp`/`Ra`/`Rp`
    relay);
  - both consoles share one public IP (same Mac), and Pia may need a working private URL then: fix nifm_ip and retest;
  - NEX NAT traversal (RequestProbeInitiationExt is never called by the joiner).
- Run used: `NAPPLE_ONLINE_NATCHECK_BASE=12025 napple-online serve --listen 127.0.0.1:8747 -v`. Two runtimes in one job under the run lock, separate
  saves and state dirs, `NAPPLE_ONLINE_NATCHECK_BASE=12025 NAPPLE_ONLINE_SERVER=http://127.0.0.1:8747`, B 5 s after A,
  `NAPPLE_INPUT_SCRIPT=15:A,19:DOWN,20.5:DOWN,22:A,32:A,36:A,40:A,44:A,48:A,53:A`.


## Summary
- **2618-0562 SessionKeepFailed: found and fixed.** After AutoMatchmakeWithParam_Postpone, MK8D waits for a "participant joined" notification
  about itself; after 10 s it unregisters and raises SessionKeepFailed. Its worldwide gathering has flags 0xa10, which include the
  verbose-participants bits 0x200 / 0x800. With those set, the server now sends 3001 to every participant, the joiner included, also when the
  joiner just created the gathering. Result: no 10-second wait, and Pia starts right away.
- **Pia's NAT check: implemented and passing.**
  - Server `natcheck.rs` answers UDP 10025 / 10125 plus the mapping ports 33334 / 33335 for nncs1, and the same ports + 1000 for nncs2.
  - Message types 1–5 and 101–103 are answered (big endian, external port / IP filled in); 2, 3 and 102 come from the documented other socket.
  - Glue:
    - `%` templates are filled at resolve (the console resolver does that), so `nncs1-%…` resolves;
    - nncs1 maps to 100.78.78.100 and nncs2 to 100.79.79.100;
    - datagrams map between the virtual servers and the server's ports in both directions, only for the NAT-check ports;
    - replies carry the virtual server address in their "server address" field.
  - MK8D runs 101 / 102 / 103, gets its answers, and reports `natm=1;natf=1` through SecureConnection ReplaceURL.
- A client on the server's own machine is reported at the LAN address (Register's public URL, NAT check replies) instead of 127.0.0.1.
- **Two MK8D instances, one job under the run lock: matched into the same gathering** (server: `matched into gathering 1 (mode 1, 2 of 12
  players)`). The joiner calls MatchMaking 41 GetSessionURLs and gets the host's station URL (`address=192.168.5.229;port=<Pia port>;natm=1;natf=1`).
- **Not done: no race lobby.** Both instances then raise **2618-0006**, about 1.3 s after matchmaking, before any peer-to-peer packet.
  - Pia's `NatDetectionJob` (main+0x966030: message id 0x3403, three Pia packets carrying `NATTestId_Dummy` to the NAT check server's port
    33335) sends from a fresh socket and closes it 5 ms later.
  - No station socket is ever opened afterwards. Echoing those packets (modified or unchanged) did not help, so the job's `StepWait` probably
    expects something else. See Findings.

## Files
- `crates/napple-online/src/server.rs`: announce_join (verbose gatherings), observed_ip in Register's public URL, test
  `verbose_gatherings_announce_every_join_to_everyone`.
- `crates/napple-online/src/matchmaking.rs`: `GATHERING_FLAG_VERBOSE_PARTICIPANTS`.
- `crates/napple-online/src/natcheck.rs` (new), `lib.rs`, `main.rs`: `NAPPLE_ONLINE_NATCHECK_BASE` (default 10025, 0 = off).
- `runtime/src/net/net_online.{h,c}`:
  - nncs redirect, second virtual address, `%` fill at resolve;
  - NAT-check datagram mapping (send / recv / connect) and the reply's server field;
  - debug: bind logging, and the guest call stack with play reports (`ipc_trace_guest`, weak).
- `runtime/tests/test_net_online_client.c`: NAT check names, template fill, a datagram round trip through the mapping (51 checks).
- Inherited from the napple-online branch after its freeze (f7c5b68..193474f): the ssl functions of `runtime/src/services/offline.c`
  (IoMode → provider, ResultWouldBlock), `hz_result.h` `HZ_R_SSL_WOULD_BLOCK`, `runtime/tests/test_net_online_ssl.c`, docs.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| `cargo test -p napple-online` | tested | 35 passed (natcheck reply fields, reply sockets, live UDP echo; verbose join notifications; ReplaceURL keeps the private URL) |
| `make -C runtime test-net-online` | tested | test_net_online_client 51/0, test_net_online_ssl 42/0 |
| `make -C runtime test test-services test-ipc-reference` | tested | exit 0 |
| SessionKeepFailed fixed on MK8D | verified | before: MatchmakeExtension 40, then 10 s of nothing, then MatchMaking 2 and play report `ErrorCode 2618-562 AppError SessionKeepFailed`; after: Pia's NAT check starts 20 ms after the matchmake answer; no SessionKeepFailed |
| Pia NAT check on MK8D | verified | server `-v`: `nat check type 101 … 102 … 103` answered; then `SecureConnection (11) method 7 -> ok` (ReplaceURL with natm=1, natf=1) |
| Two instances in one gathering | verified (server side) | one job, A then B 0.6 s later: `pid …745 matched into gathering 1 (1 of 12)`, `pid …746 matched into gathering 1 (2 of 12)`; with the nifm fix below also `MatchMaking (21) method 41 -> ok` returning A's URL |
| Two instances in one race lobby | not done | both raise 2618-0006 at ~37.5 s (play reports) |

## Findings
- Notification rule (decision): a verbose gathering notifies every participant, the joiner included. Otherwise only the owner is notified, and
  not about itself. Sent after the RMC answer (session tick order).
- NAT check:
  - The public docs give layouts and types only. The socket each type is answered from follows the wiki.
  - The mapping ports 33334 / 33335 are hard-coded in MK8D (main+0x9621c8 `mov w2, #0x8236`, five zero-filled packets: NAT mapping openers;
    no answer is needed).
  - Anything that is not a NAT check type is dropped.
- **nifm GetCurrentIpAddress byte order (not mine; mk8d-lan-session owns those rows).**
  - `nifm_ip` writes the host-order IP. MK8D sees 229.5.168.192 for 192.168.5.229 (a multicast address).
  - With a local `__builtin_bswap32` in `nifm_ip`, the joiner got as far as GetSessionURLs; without it, it did not.
  - Needed for peer-to-peer.
- 2618-0006 (open):
  - Module 618 description 6, raised after Pia's `NatDetectionJob` (job names at main string 0xf0bdea…0xf0be73: StepPreTest / Start / Send /
    Wait / Complete / End).
  - The job sends message 0x3403 (`NATTestId_Dummy`) to nncs1:33335 from a fresh socket on the NAT-check port, then closes it.
  - The failure is decided within ~650 ms with no IPC, for host and joiner alike.
  - Things tried without effect:
    - server echo of those packets, modified or unchanged;
    - the virtual server address in reply field 0xC;
    - LAN address instead of loopback;
    - nifm IP byte order alone;
    - a GetCurrentNetworkProfile answer (round 7).
  - Next step: disassemble `NatDetectionJob::StepWait` / `StepComplete` around main+0x9660f4–0x966140 and the callers of 0x9a1d04 / 0x9a4718
    (Pia send helpers). Find what the wait checks: likely a Pia packet that should arrive on the station from the NAT check server, or a timer
    comparison at `[x20,#0xdf8]` / `[x20,#0xe58]`.
- Debug aids (committed): `NAPPLE_ONLINE_DEBUG=1` logs binds, datagram mapping and play reports with the guest call stack. The error payload is
  in prepo's second buffer, which `prepo_deliver` does not pass; a local one-line dump was used (not committed).

## How to integrate
- Merge after `claude/napple-online-2026-11-02-r7` (this branch contains it plus 7dde717..193474f).
- Conflict hotspots:
  - `offline.c`: ssl functions only (7dde717); mk8d-lan-session edits the nifm rows of the same file;
  - `online_provider.h`: unchanged here.
- Build: `cargo build --release -p napple-online`, `make -C runtime` (relink after tests: the test targets do not relink `napple-runtime`).
- Infra: the server now also needs UDP 10025, 10125, 33334, 33335, 11025, 11125, 34334, 34335 (or `NAPPLE_ONLINE_NATCHECK_BASE=0` to disable).
  Behind a NAT or a proxy the server must see clients' real addresses for the NAT check.

## How to test
1. `cargo test -p napple-online` → 35 passed. `make -C runtime test-net-online` → 51/0 and 42/0.
2. One instance:
   ```bash
   target/release/napple-online serve -v
   NAPPLE_NET=host NAPPLE_ONLINE_SERVER=http://127.0.0.1:8737 NAPPLE_ONLINE_DEBUG=1 \
     NAPPLE_INPUT_SCRIPT=15:A,19:DOWN,20.5:DOWN,22:A,32:A,36:A,40:A,44:A \
     runtime/build/napple-runtime <MK8D data> --headless --seconds 62 --save-root <copy of a save past first boot>
   ```
   - Passing for this branch: the server log shows `method 40 -> ok`, then NAT check types 101 / 102 / 103, then `method 7 -> ok`, with no
     MatchMaking 2 ten seconds later.
   - Expected failure today: the error applet at ~37.5 s, play report `ErrorCode 2618-6`.
3. Two instances: two runtimes with separate `--save-root` and `NAPPLE_ONLINE_STATE_DIR`, the second started ~0.6 s after the first, inside
   one `lockf -k /Users/neto/NappleWork/.run.lock` job → both `matched into gathering 1`.

## Needs from other areas
1. nifm (mk8d-lan-session): GetCurrentIpAddress in network byte order (`__builtin_bswap32(ni.ip)` / htonl in `nifm_ip`), verified to move the
   joiner forward. GetCurrentIpConfigInfo with the same convention.
2. sfdnsres: hostent IPv4 byte order (still worked around with palindromic virtual addresses).
3. prepo: pass both buffers to the provider's `report` hook.
4. infra: the UDP ports above.
