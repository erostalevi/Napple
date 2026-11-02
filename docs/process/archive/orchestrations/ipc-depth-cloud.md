---
name: ipc-depth-cloud
batch: 2026-11-02-r9
branch: claude/ipc-depth-cloud-2026-11-02-r9
base: origin/claude/ipc-depth-cloud-2026-11-02-r9 rebased by the orchestrator onto local main (21e785e); originally from publish 57431a7
env: linux-cloud
model: claude-opus-5-5
depends_on: []
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: cloud (task_f5406b19)
launched_at: 2026-11-02
---

# ipc-depth-cloud (cloud session; brief was given inline, see the integrator file on the branch)

## Acceptance (for the Mac integrator)
- Every code commit is `unbuilt:` (Linux only): build on macOS, `make -C runtime GFX=napple test test-services test-ipc-reference` (missing 0), four-title boots; 98 rows made real (11 stub->impl, 87 new impl rows; new sysset_core/notif_core/pctl_core).

## Result
Integrated 2026-11-02 into main as fc04ba1 (head 3b5e0b7), outcome **Clean**: built on macOS without changes, suites green (missing 0), four-title boots
unchanged. Details: [integrator file](../integrators/claude-ipc-depth-cloud-2026-11-02-r9.md).
