---
branch: claude/online-race-2026-11-02-r7
orchestration: online-race (follow-up assignment by message from the orchestrator; no orchestrations/ file)
base: adf717a (claude/online-pia-2026-11-02-r7 head)
head: 99a5555
env: mac
date: 2026-11-02
areas: [crates/napple-online, runtime/services (offline.c nifm_ip row only), runtime/tests]
status: ready-with-caveats
---

# claude/online-race-2026-11-02-r7: the MK8D joiner reaches NAT traversal with the host; not yet the race lobby

## Summary
Worked the suspect list in order of evidence. Two MK8D instances, one job under the run lock, local napple-online:
1. **nifm GetCurrentIpAddress byte order: fixed** (260cac6, 9ca2c7b). `nifm_ip` now answers an in_addr (`htonl`), like the fixed
   GetCurrentIpConfigInfo. MK8D used to put 229.5.168.192 for 192.168.5.229 into its station URL.
   - Diff limited to the `nifm_ip` row of `offline.c`; ipc-depth-95 converts other rows.
   - Three tests pinned the host-order value and now expect network order: `test_net_host_sock`, and two checks in `test_svc_online`.
2. **Station URL keys / what the joiner expects (47067ce):**
   - The public URL from Register now carries `type=3` (reachable public address | behind a NAT). With it, the joiner starts NEX NAT traversal
     (RequestProbeInitiationExt) instead of quitting right after GetSessionURLs.
   - With the correct local IP, MK8D does not publish its station (no ReplaceURL) when the server already reports it as reachable, so the
     host's URLs keep the transport's port 1.
   - The server now fills in the host's station port from its NAT check: the NAT check records (address, external port, time); a client gets
     the earliest unclaimed port from its address, and a client's own URL in RequestProbeInitiationExt / ReplaceURL claims its port. The port
     goes into every URL, the private one included, with `natm=1;natf=1;type=3`.
   - The private URL matters because a joiner behind the same public address as the host (two instances on one Mac, or one LAN) probes the
     host's private URL. Given only the public one, it built an empty target.
3. **NEX probe initiation: works.**
   - Joiner: GetSessionURLs, then RequestProbeInitiationExt ×2, targeting the host's private URL with the real port.
   - The server forwards InitiateProbe to the host; the host probes the joiner and also calls RequestProbeInitiationExt; the joiner is
     probed back.
   - **The probe exchange completes in both directions at the right addresses** (2 + 2 + 2 + 2 Pia NAT probe packets, 84 bytes).
4. **Not reached: the race lobby.** About 150 ms after the probe exchange the joiner calls MatchMakingExt EndParticipation without sending a Pia
   connection request, and shows **2618-0502**. The host keeps waiting (no error) until the run ends.


## Update (second round on this branch)
Further two-instance runs (same job setup as below):
- **Reported at loopback** (`NAPPLE_ONLINE_PUBLIC_IP=127.0.0.1` on the server): each console's public address (127.0.0.1) then differs from its
  local one (192.168.5.229), as behind a home NAT.
  - Both consoles publish their station with SecureConnection ReplaceURL; the call replaces their *private* URL with
    `address=192.168.5.229;port=<Pia port>;natf=1;natm=1`.
  - GetSessionURLs gives the joiner `[private 192.168.5.229:<port>, public 127.0.0.1:<port>]` (both with `type=3`, port filled from the claim).
  - The joiner probes the host's private URL, and the probe exchange completes. Then the same 2618-0502 ~150 ms later.
  - So the outcome does not depend on whether the host publishes its station or the server fills it in.
- **CID.** The joiner names the host with `CID=0`; the host's own URL (in its RequestProbeInitiationExt) carries a random CID. Tried:
  - copying it into handed-out URLs once known;
  - sending the host an InitiateProbe naming itself when it publishes, to make it reveal its CID before a joiner arrives. The host does not
    answer that with RequestProbeInitiationExt, so its CID is not known before the joiner's GetSessionURLs.
  - Reverted (no effect, speculative). A host's ReplaceURL never carries a CID either, so a real server would not have it: probably not the cause.
- **Not done: a distinct public address per console on one Mac.** It would put the joiner on Pia's normal internet path instead of the
  same-NAT path. It cannot be made consistent with the NAT check, which only sees (address, port).
- **Best next steps:**
  1. **Two Macs on one LAN**, server on one of them: each console has its own address, so no same-NAT special case, and the server reports each
     at its own LAN address. If the race lobby works there, the remaining problem is specific to two instances behind one address.
  2. **Disassemble Pia's joiner path after a successful probe** (InetConnectStationJob / the job that calls EndParticipation; strings near main
     0xf0bdea, "ConnectStationJob::SendRelayConnectionRequest", "InetConnectStationJob::PrepareNatTraversalByRelay") to see what it checks.
     Also implement GetRelaySignatureKey (NATTraversal 6, already answered) and relay settings, if the joiner wants a relay route when the
     direct one fails.
  3. Read the joiner's Pia packets after the probes (none are sent: the decision is local).

## Update (third round: disassembly of the joiner path)
- **Error reporting path (game side).** The joiner's error play report is raised from the game's UI-level handler (guest stack: sdk+0x167540 →
  main+0x38aa0c → main+0x498a04 → 0x498314, a struct copy of the error info). That code only forwards a Pia failure; it does not decide it.
- **Pia side, located but not traced to the failing condition.** The connect jobs sit at main 0x932xxx–0x933014 (`InetConnectStationJob`:
  TestCurrentAddress / ResolveCurrentAddress / PrepareNatTraversal / WaitForNatTraversalStarted / WaitForNatTraversalCompleted /
  TryCurrentAddress, and the `...ByRelay` variants) and 0x99dd..–0x99e5.. (`ConnectStationJob::ConnectionFailed` ×4 call sites, SendConnectionRequest
  at 0x933014 / 0x9709f4 / 0x99ddc4). The state function at 0x9328f0–0x932abc handles 15000 ms timeouts and a retry counter
  (`[x19+0x15c]`), and sets error code 0x644a (25674) on exhaustion. The joiner fails after ~150 ms, so a different branch (a
  `ConnectionFailed` site) fires: the next step is to hook those call sites (NAPPLE_PCPROF or a guest breakpoint on 0x99df54 / 0x99e294 /
  0x99e420 / 0x99e580 / 0x932aa8 / 0x93285c / 0x932f60) to see which one, and read the registers that feed its compare.
- **Experiment, no effect (reverted):** not relaying the server's InitiateProbe to a station that has itself just asked to probe the caller.
- Tool added for this: `scratchpad xref.py` style string-xref (adrp+add) over an `llvm-objdump` of `mkelf.py`'s ELF; see
  `napple-online-r7/` notes. Not committed.
- Blocking condition, as far as known: after a *successful* probe exchange the joiner's `InetConnectStationJob` reports failure without
  sending `SendConnectionRequest`, i.e. before its own connection step, from a path that does not wait for a timeout.

## Final pass (time-boxed): what the ConnectionFailed sites are
- The `adrp/add` sites that reference the step-name strings (0x99ddc4 → `SendConnectionRequest`, 0x99df54 → `ConnectionFailed`, 0x932aa8 etc.)
  store the job's *next state name* in the job object (`[x20+0x40]`) together with a result code in w0/x0 (0 = continue, 5 = wait) and then
  return. They are state transitions of the `InetConnectStationJob` step machine, not the failure conditions. The conditions are the
  branches that reach them (`b 0x99df44` → `0x99df54`).
- So the failing branch cannot be found by reading call sites alone. It needs a dynamic hook: break at 0x99df44/0x99df54 (or log the value
  stored at `[job+0x40]` on every step) and take the caller's registers. The runtime has no guest breakpoint facility; `NAPPLE_WATCH`
  (store watch on `<module>+<offset>`) on the job's `[+0x40]` field would log every state change with pc and value: that is the cheapest next
  step (find the job object address from a guest stack at the error report, `x19`/`x20` in the functions above).
- Everything needed to reproduce and continue is committed: two-instance job setup, ports, logs to read, and the evidence sequence.

## Files
- `runtime/src/services/offline.c`: `nifm_ip` only.
- `runtime/tests/test_net_host_sock.c`, `runtime/tests/test_svc_online.c`: the host-order expectations.
- `crates/napple-online/src/server.rs`:
  - `type=3` on Register's public URL;
  - `Client::station_port` / `station_urls()`, `fill_station`;
  - ReplaceURL stamps `type=3` and claims the port;
  - RequestProbeInitiationExt claims the caller's port;
  - GetSessionURL(s) hand out `station_urls()`.
- `crates/napple-online/src/natcheck.rs`: `record` / `station_port` (the NAT check's view of each station), LAN-address reporting for
  same-machine clients (unchanged from online-pia).

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| `cargo test -p napple-online` | tested | 36 passed (+ station_ports) |
| `make -C runtime test test-services test-ipc-reference` | tested | exit 0 (after the three expectation updates) |
| `make -C runtime test-net-online` | tested | 51/0, 42/0 |
| nifm IP in network order on MK8D | verified | the station URL in RequestProbeInitiationExt / GetSessionURLs says `address=192.168.5.229` |
| Joiner does NAT traversal with the host | verified | server `-v`: `MatchMaking (21) method 41`, `NATTraversal (3) method 3` ×2 (joiner) and ×1 (host); `NAPPLE_NET_LOG`: host ↔ joiner probe packets both ways at the host's private URL port |
| Two instances in the race lobby / a race | not done | joiner play report `ErrorCode 2618-502` ~150 ms after the probes |

## Findings
- Port collisions with other sessions' napple-online servers: the integration session uses 8737 / UDP 10025…; online-friends ran one on 18737
  with NAT check base 12025. A test that silently talks to another server gives meaningless results.
  - Use `--listen 127.0.0.1:8767` and `NAPPLE_ONLINE_NATCHECK_BASE=22025` on both server and runtime.
  - Check the server's second log line (`NAT check on UDP […]`, not `NAT check not started`).
- The sequence of evidence, for the next round:
  - Without `type` on the public URL, the joiner quits right after GetSessionURLs (or, with a correct local IP, both fail 2618-0201 ~14 s later
    with no GetSessionURLs).
  - With `type=3` and only the transport URL (port 1), the joiner probes, but its target has port 1.
  - With only the public URL carrying the real port, the joiner's target is empty: it wants the private URL.
  - With the private URL carrying the real port (+ natm/natf/type), the probes complete at the right addresses, and then 2618-0502.
- Next suspects for 2618-0502 after a successful probe:
  - Pia's station connection step (`InetConnectStationJob`, strings near main 0xf0bdea) checking something the server should have provided: a
    relay setting (GetRelaySignatureKey is never called), the `Rsa`/`Rsp` relay keys (all 0 in the joiner's URLs), or the `CID` in the host's
    URL. The joiner's own URL has a random CID; the host URL we hand out has none, so the joiner writes `CID=0`.
  - Next step: give the host URL its CID (the host's own RequestProbeInitiationExt URL has `CID=2264964919`), or carry the host's own URL
    verbatim once known, and retest.
  - The joiner calls RequestProbeInitiationExt before the host's URL is learned from the host, so the server would have to learn the host's
    CID earlier: Register / RegisterEx custom data, or the host's first probe.
- Debug aids used (not committed): the prepo second-buffer dump in `prepo_deliver` (gives `ErrorCode` from the error play report).

## How to integrate
- Merge after `claude/online-pia-2026-11-02-r7` (this branch starts at its head adf717a).
- Conflict hotspots:
  - `offline.c`: only the `nifm_ip` row (ipc-depth-95 converts other rows; mk8d-lan-session's nifm rows are already merged here);
  - `runtime/tests/test_svc_online.c`: two lines.
- Build: `cargo build --release -p napple-online`; `make -C runtime`. The test targets do not relink `napple-runtime`; run `make -C runtime` before
  game runs.
- Infra: the NAT check UDP ports (10025, 10125, 33334, 33335 and +1000) as in online-pia.

## How to test
1. `cargo test -p napple-online` (36). `make -C runtime test test-services test-ipc-reference` (exit 0).
2. Two instances: start the server, then the two runtimes from one script under `lockf -k /Users/neto/NappleWork/.run.lock`. Each gets its own
   `--save-root` (a copy of a save past first boot) and its own `NAPPLE_ONLINE_STATE_DIR`. B starts 5 s after A.
   ```bash
   NAPPLE_ONLINE_NATCHECK_BASE=22025 target/release/napple-online serve --listen 127.0.0.1:8767 -v
   # per instance:
   NAPPLE_NET=host NAPPLE_ONLINE_SERVER=http://127.0.0.1:8767 NAPPLE_ONLINE_NATCHECK_BASE=22025 NAPPLE_ONLINE_DEBUG=1 NAPPLE_NET_LOG=1 \
     NAPPLE_INPUT_SCRIPT=15:A,19:DOWN,20.5:DOWN,22:A,32:A,36:A,40:A,44:A,48:A,53:A \
     runtime/build/napple-runtime <MK8D data> --headless --seconds 95 --save-root <copy>
   ```
   - Passing for this branch: both `matched into gathering 1`; the joiner's `MatchMaking (21) method 41` and `NATTraversal (3) method 3`; probe
     packets in both logs (`net: send(1, 84 bytes … to 192.168.x.y:<port>`).
   - Expected today: joiner error 2618-0502, host waits.

## Fix policy hints
- Safe: the station-port association heuristics (one public address shared by several consoles) and URL key stamping.
- Design decisions: whether the server should rewrite station URLs at all, or require clients to publish their station. MK8D skips publishing
  when the server reports it as reachable, so a real deployment on a LAN hits the same case.

## Needs from other areas
- None blocking from other sessions for this step. prepo's second buffer to the provider would make the error code visible without a local patch.
