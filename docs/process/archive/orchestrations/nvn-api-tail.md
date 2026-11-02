---
name: nvn-api-tail
batch: 2026-11-03-r11
branch: claude/nvn-api-tail-2026-11-03-r11
base: orchestrate/2026-11-03-r11
env: mac
model: claude-sonnet-5-5
depends_on: []
status: integrated
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: pending
launched_at: 2026-11-03
---

# nvn-api-tail: NVN entry points the titles look up but we lack

## Owned paths
`gfx/src/api_texture.c`, `api_device.c`, `api_program.c`, `gfx/src/nvnapi/names.txt` and `nvn_protos*.h` (append), `gfx/tests/t_nvn_ext.c`, `tools/gen_a32_nvn.py` + `runtime/src/a32/a32_nvn_sigs.inc` (regenerate only)

## Do not touch
Everything else (other sessions edit runtime/src/services, runtime/src/net, gfx shader/replay/stages, media in parallel), `orchestrations/`.

## Work and acceptance
1. Check which of nvnTextureBuilderGetViewOffset, nvnDeviceWaitForError, nvnProgramGetSubtileSize are not yet real implementations (r2 added names; some may only be inferred stubs) and make each a real implementation with a test; add the names + definitions in the same commit so the NVN ratio never drops.
2. Regenerate a32 sigs; four-title boots show no 'NVN lookup of unknown name'.
3. `make -C gfx test` green.

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data read-only, lockf run lock, local-only, never ask, never push). Model: Sonnet 5.5, medium effort: work test-first in small steps, commit each.

## Result

Outcome: Clean. Merged commit 5e50896. Docs-only hand-off (assignment already implemented before the round, no source change); nothing to fix, no native port.
Integrator record: docs/process/archive/integrators/claude-nvn-api-tail-2026-11-03-r11.md
