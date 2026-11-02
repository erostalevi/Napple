---
branch: claude/online-race-2026-11-02-r7
orchestration: online-race (follow-up assignment by message from the orchestrator; no orchestrations/ file)
base: adf717a (claude/online-pia-2026-11-02-r7 head)
head: HEAD_AT_HANDOFF
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
