---
name: ipc-depth-95
batch: 2026-11-02-r10
branch: claude/ipc-depth-95-2026-11-02-r10
base: orchestrate/2026-11-02-r10 (main after mk8d-lan-session); merge main again before hand-off (integration of r7-r9 is landing)
env: mac
model: claude-opus-5-5
depends_on: []
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_a3121690-1a9f-4389-a496-a7b6c07862df (fresh)
launched_at: 2026-11-02
---

# ipc-depth-95: IPC command depth from 91% to at least 98% (user target: every sub-item 98-99%, overall ~99.5%)

## Goal
%Analysis measures IPC depth from runtime/tests/ipc_reference (impl 1 / stub 0.5 / missing 0) over 1325 app-reachable commands (+ rows without
fixtures). It was 91% judged (snapshot 2026-11-02-8389280). ipc-depth-cloud (98 rows, landing now) helps; this item takes it to >= 98%:
convert the remaining documented-default `IPC_STUB` rows to real behaviour, including the NETWORK rows, which now have a real path: with
`NAPPLE_NET=host` they go through the host provider / Napple online glue, and offline they answer exactly what an offline console answers —
implement both paths and mark them IPC_IMPL with tests. Priority by game usage: nifm, bsd, sfdnsres, ssl, nsd, friend, prepo, account (Napple
online path), fs.c leftovers, applet (am) leftovers, ldn, bcat, npns. Run `python3 -I .claude/skills/percent-analysis/scan.py` before/after and
report the IPC depth numbers.

## Owned paths
runtime/src/services/ (all service files) EXCEPT: rows being edited by online-friends (friend rows) — coordinate by keeping friend:u for last and
merging main/online-friends if they land first; runtime/tests/; docs/architecture/runtime-services.md (rows in place). Not runtime/src/net/
(online-friends), not gfx/, not crates/.

## Acceptance
1. scan.py IPC depth >= 98% measured (and the fixtures still report missing 0).
2. Every converted row has a unit test; `make -C runtime GFX=napple test test-services test-ipc-reference` green.
3. Four-title boots (with and without NAPPLE_NET=host) unchanged: rc 0, missing_ipc [].

## Env notes
Read orchestrations/ENV-MAC.md. Layout facts from switchbrew/libnx only, no GPL code.
Integrated 2026-11-02 into main as 5d0c5ec (head 98dc102), outcome **Clean**: IPC depth 98% measured, suites green, LAN / online regression runs unchanged,
four-title boots unchanged. Details: [integrator file](../integrators/claude-ipc-depth-95-2026-11-02-r10.md).
