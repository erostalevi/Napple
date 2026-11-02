---
branch: claude/gfx-xfb-tess-audit-2026-11-03-r11
orchestration: gfx-xfb-tess-audit
base: 281f4d3483540816dea22517d4d3d9a263adfb67
head: pending
env: mac
date: 2026-11-03
areas: [gfx]
status: ready-with-caveats
---

# claude/gfx-xfb-tess-audit-2026-11-03-r11: XFB / tessellation encodings vs public Maxwell header

## Summary
Compared the tess parameter word and the transform-feedback layout encodings with NVIDIA open-gpu-doc clb197.h. XFB stride/count/layout-select packing matches. The tess word was wrong in the output-primitives field (bit 8 was read as clockwise; the field is 9:8: points 0, lines 1, CW 2, CCW 3): fixed in control.c, points refused, lines tied to isoline. Pinned by new t_tess_xfb_layout (source in comments). The 4 long-standing SKIPs (t_feedback 1, t_clear_ds 1, t_clear_tex 2) are NOT fixed: the causes are in gfx/src/mtl_replay.m (not owned); exact reasons are in docs/architecture/gfx.md. Decision: document, not edit, to avoid colliding with other sessions. Hardware offset of the control-section blocks stays a hypothesis.

## Files
gfx/src/shader/control.c, control.h (comment), gfx/tests/t_control.c, new gfx/tests/t_tess_xfb_layout.c, docs/architecture/gfx.md (append), docs/archive/changes note, this file.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| 1 compare encodings, fix mismatches | verified (against the public header) | tess winding fixed |
| 2 tests citing sources | tested | t_tess_xfb_layout ok, t_control ok |
| 3 close SKIPs | documented, not fixed | needs mtl_replay.m change |
| 4 make -C gfx test | verified | rc=0, ALL TESTS PASSED |
| four-title boots unchanged | untested | no boot run: the changed reader finds no tess word in any title (no title has tess eval programs), so no behaviour change expected |

## Unbuilt / untested list
None unbuilt.

## Findings
- No title has tess programs, so the corrected winding is unexercised by real data.
- SKIP causes: t_feedback/t_clear_ds: clear after a readback is folded (do_clear_color/do_clear_ds) and flush_clears() returns early when P.open; t_clear_tex: do_clear_tex ignores region and mask (void)mask.
- Clear-fold fix location: mtl_replay.m (owner other session).

## How to integrate
No ordering constraints. Hotspot: docs/architecture/gfx.md (append-only). Build: make -C gfx test.

## How to test
1. make -C gfx build/tests/t_tess_xfb_layout build/tests/t_control; run both: "ok".
2. make -C gfx test: ALL TESTS PASSED, 4 skips.

## Fix policy hints
Safe: nothing pending. The SKIP fixes need a design decision in mtl_replay.m owners.

## Needs from other areas
mtl_replay.m: fix the two clear/readback and clear_tex region+mask behaviours above, then turn the KNOWN into CHECK.
