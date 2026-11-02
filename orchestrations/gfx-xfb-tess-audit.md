---
name: gfx-xfb-tess-audit
batch: 2026-11-03-r11
branch: claude/gfx-xfb-tess-audit-2026-11-03-r11
base: orchestrate/2026-11-03-r11
env: mac
model: claude-sonnet-5-5
depends_on: []
status: launched
orchestrator_session: local_67fc7cb9-f351-4dc9-aa12-15775cd6e97a
session: pending
launched_at: 2026-11-03
---

# gfx-xfb-tess-audit: check transform-feedback and tessellation layouts against public NVIDIA documentation and clear the known test skips

## Owned paths
`gfx/src/mtl_stages.inc`, `gfx/src/shader/control.c`, `gfx/tests/t_feedback.c`, `t_clear_ds.c`, `t_clear_tex.c`, new `gfx/tests/t_tess_xfb_layout.c`, `docs/architecture/gfx.md` (append)

## Do not touch
Everything else (other sessions edit runtime/src/services, runtime/src/net, gfx shader/replay/stages, media in parallel), `orchestrations/`.

## Work and acceptance
1. Compare the XFB control-memory layout and the tessellation parameter/domain/spacing/winding encodings with the public Maxwell class headers (NVIDIA open-gpu-doc, Nouveau/envytools register docs: layout facts only, never copy GPL code) and fix mismatches.
2. Pin each encoding with a test citing its source in a comment.
3. Investigate and close the long-standing SKIPs in t_feedback (1), t_clear_ds (1), t_clear_tex (2): fix or document the exact reason each cannot pass.
4. `make -C gfx test` green; four-title boots unchanged.

## Env notes
Read `orchestrations/ENV-MAC.md` first (shared title data read-only, lockf run lock, local-only, never ask, never push). Model: Sonnet 5.5, medium effort: work test-first in small steps, commit each.

## Result
