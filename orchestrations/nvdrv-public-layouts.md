---
name: nvdrv-public-layouts
batch: 2026-11-02-r10
branch: claude/nvdrv-public-layouts-2026-11-02-r10
base: origin/claude/nvdrv-public-layouts-2026-11-02-r10 rebased by the orchestrator onto local main; originally from publish d0d4be9
env: linux-cloud
model: claude-opus-5-5
depends_on: []
status: handed-off
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: cloud (task_b3038706)
launched_at: 2026-11-02
---

# nvdrv-public-layouts (cloud session; brief inline, see the integrator file on the branch)

## Acceptance (Mac integrator)
- unbuilt: code (Linux only): build on macOS, make -C runtime GFX=napple test test-services, four-title boots; MK8D NVDEC/VIC preview still decodes; verify docs/reference/nvdrv-layouts.md sources.

## Result
