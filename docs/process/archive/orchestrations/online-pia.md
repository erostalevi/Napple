---
name: online-pia
batch: 2026-11-02-r7
branch: claude/online-pia-2026-11-02-r7
base: claude/napple-online-2026-11-02-r7 (lands after it; merges mk8d-lan-session for the nifm rows)
env: mac
model: claude-opus-5-5
depends_on: [napple-online, mk8d-lan-session]
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: local_8e7ca377-aa50-49b2-bdc8-9625b3743e48 (reused napple-online)
launched_at: 2026-11-02
---

# online-pia: MK8D gets past SessionKeepFailed (2618-0562) and two instances race online through napple-online

Owned: crates/napple-online, runtime/src/net/net_online*, ssl functions of runtime/src/services/offline.c. nifm rows come from mk8d-lan-session (merge it).
Acceptance: root cause of 2618-0562 found and fixed generically; two MK8D instances matched into the same online race lobby through a local napple-online
(verified, one job under the run lock); tests green; four-title boots unchanged.

## Result
Integrated 2026-11-02 into main as 683e9bf (head adf717a), outcome **Clean**. SessionKeepFailed 2618-0562 fixed and Pia's NAT check passes (verified on
MK8D); two instances verified matched into the same gathering through a local napple-online; the host reaches the course vote, the joiner still ends with
2618-0502 (open, follow-up running). Details: [integrator file](../integrators/claude-online-pia-2026-11-02-r7.md).
