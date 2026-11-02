---
name: nvdrv-public-layouts
batch: 2026-11-02-r10
branch: claude/nvdrv-public-layouts-2026-11-02-r10
base: origin/claude/nvdrv-public-layouts-2026-11-02-r10 rebased by the orchestrator onto local main; originally from publish d0d4be9
env: linux-cloud
model: claude-opus-5-5
depends_on: []
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: cloud (task_b3038706)
launched_at: 2026-11-02
---

# nvdrv-public-layouts (cloud session; brief inline, see the integrator file on the branch)

## Acceptance (Mac integrator)
- unbuilt: code (Linux only): build on macOS, make -C runtime GFX=napple test test-services, four-title boots; MK8D NVDEC/VIC preview still decodes; verify docs/reference/nvdrv-layouts.md sources.

## Result
Integrated 2026-11-02 into main as 52637d5 (head c6f2597), outcome **Clean**: builds on macOS, suites green, MK8D NVDEC/VIC preview verified, four-title boots
unchanged. Details: [integrator file](../integrators/claude-nvdrv-public-layouts-2026-11-02-r10.md).
