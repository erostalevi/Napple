---
name: online-support-table
batch: 2026-11-02-r10
branch: claude/online-support-table-2026-11-02-r10
base: origin/claude/online-support-table-2026-11-02-r10 rebased by the orchestrator onto local main; originally from publish d0d4be9
env: linux-cloud
model: claude-opus-5-5
depends_on: []
status: handed-off
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: cloud (task_a28d9729)
launched_at: 2026-11-02
---

# online-support-table (cloud session; brief inline, see the integrator file on the branch)

## Acceptance (Mac integrator)
- python3 tools/support/gen_support.py --check, tools/check_docs.py; README online block + site section render; update compat/online.json with the latest MK8D online results at integration time (LAN Play works, online matchmaking works, race pending, friends pending).

## Result
