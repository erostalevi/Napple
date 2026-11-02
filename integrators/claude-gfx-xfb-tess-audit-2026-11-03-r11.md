---
branch: claude/gfx-xfb-tess-audit-2026-11-03-r11
orchestration: gfx-xfb-tess-audit
base: 281f4d3483540816dea22517d4d3d9a263adfb67
head: da821a7
env: mac
date: 2026-11-03
areas: [gfx]
status: ready
---

# claude/gfx-xfb-tess-audit-2026-11-03-r11: XFB / tessellation encodings vs public Maxwell header

## Summary
Compared the tess parameter word and the transform-feedback layout encodings with NVIDIA open-gpu-doc clb197.h. XFB stride/count/layout-select packing matches. The tess word was wrong in the output-primitives field (bit 8 was read as clockwise; the field is 9:8: points 0, lines 1, CW 2, CCW 3): fixed in control.c, points refused, lines tied to isoline. Pinned by new t_tess_xfb_layout (source in comments). The 4 long-standing SKIPs (t_feedback 1, t_clear_ds 1, t_clear_tex 2) are FIXED in gfx/src/mtl_replay.m (ownership granted by the orchestrator mid-task): blit() ends the open blit encoder when a folded clear is pending; do_clear_tex honours region and channel mask via a clear quad (clear_tex_pipe). The KNOWN checks became CHECKs. Hardware offset of the control-section blocks stays a hypothesis.

## Files
gfx/src/shader/control.c, control.h (comment), gfx/src/mtl_replay.m, gfx/tests/t_control.c, t_feedback.c, t_clear_ds.c, t_clear_tex.c, new gfx/tests/t_tess_xfb_layout.c, docs/architecture/gfx.md (append), docs/archive/changes note, this file.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| 1 compare encodings, fix mismatches | verified (against the public header) | tess winding fixed |
| 2 tests citing sources | tested | t_tess_xfb_layout ok, t_control ok |
| 3 close SKIPs | verified | 0 skipped in all three tests |
| 4 make -C gfx test | verified | rc=0, ALL TESTS PASSED |
| four-title boots | verified | SMO/LAS/MK8D 60 s, ACNH 90 s, MTL_DEBUG_LAYER=1, run lock, private save root: exit 0, 0 unimplemented NVN calls, no validation errors |
| LAS shader corpus | verified | t_xlate: 5767 programs, 5767 translated, 5767 compile |

## Unbuilt / untested list
None unbuilt.

## Findings
- No title has tess programs, so the corrected winding is unexercised by real data.
- Skip causes and fixes: see gfx.md 'Former known skips, closed'. ClearTexture with mask 0 now writes nothing (it cleared everything before); depth/3D/1D targets keep the whole-subresource clear.

## How to integrate
No ordering constraints. Hotspot: docs/architecture/gfx.md (append-only). Build: make -C gfx test.

## How to test
1. make -C gfx build/tests/t_tess_xfb_layout build/tests/t_control; run both: "ok".
2. make -C gfx test: ALL TESTS PASSED, 4 skips.

## Fix policy hints
Conflicts in mtl_replay.m blit()/do_clear_tex: keep both sides; the pending-clear check must precede the g_be test.

## Needs from other areas
Nothing.
