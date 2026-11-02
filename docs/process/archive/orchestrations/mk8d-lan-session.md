---
name: mk8d-lan-session
batch: 2026-11-02-r7
branch: claude/mk8d-lan-session-2026-11-02-r7
base: claude/net-rooms-2026-11-02-r7 (lands after it)
env: mac
model: claude-opus-5-5
depends_on: [net-rooms]
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_d8199ac5-b4e7-45f0-8d48-22e3a3ab703e (reused net-rooms)
launched_at: 2026-11-02
---

# mk8d-lan-session: two MK8D instances meet in LAN Play / Wireless Play (fix error 2618-0006)

## Goal
net-rooms proved the relay: each instance receives the other's LAN announcement. MK8D then aborts with 2618-0006 ~2 s into the session search, also
on a real LAN without rooms and in Wireless Play, so the cause is in our HLE services. Known blockers: nifm `GetCurrentIpConfigInfo` is a stub (LAN Play
cannot start without it) and `GetPseudoDeviceId` is identical for every instance. Find and fix the real cause(s); verify two instances in the same lobby.

## Owned paths
nifm rows in `runtime/src/services/offline.c` / `offline_core.c` (GetCurrentIpConfigInfo, GetCurrentNetworkProfile and friends, consistent with the host
provider), the am `GetPseudoDeviceId` row in `runtime/src/services/applet.c` (unique per installation + profile, stable across runs, stored in the
system save area), any other service row the 2618-0006 trace points at (list each in the integrator file), `runtime/src/net/` (bug fixes),
their tests, own change note. Do not touch gfx/, crates/ other than napple-room bug fixes, orchestrations/.

## Acceptance
1. Root cause of 2618-0006 identified with an IPC/socket trace (NAPPLE_TRACE_IPC=all, NAPPLE_NET_LOG=1) and fixed generically.
2. **Verified**: two MK8D (update) instances on this Mac, via a local napple-room, both in the same LAN Play lobby seeing each other (screenshots/logs); and
   Wireless Play (LDN) through the room if it uses a different path. One job under the run lock, ≤120 s.
3. `make -C runtime test test-services test-ipc-reference` green; four-title boots unchanged.

## Env notes
Read `orchestrations/ENV-MAC.md` (shared title data read-only, locks, local-only, never ask, never push).

## Result
Integrated 2026-11-02 into main as 33ac45c (head e6a4dd9), outcome **Clean**. Acceptance 1 and 3 checked; acceptance 2 verified here for LAN Play (two MK8D
instances in the same room's character select through a local napple-room); Wireless Play still crashes the station after Connect (not diagnosed).
Details: [integrator file](../integrators/claude-mk8d-lan-session-2026-11-02-r7.md).
