# gfx-xfb-tess-audit (2026-11-03-r11)
- `gfx/src/shader/control.c`: tess parameter word decodes OUTPUT_PRIMITIVES 9:8 per clb197.h (was bit 8 = cw); refuses points / inconsistent domain+primitive. `control.h` comment updated.
- New `gfx/tests/t_tess_xfb_layout.c`; `t_control.c` corrected for the new word. Findings in `docs/architecture/gfx.md` (appended).
- Verified: `make -C gfx test` green (Mac). Skips unchanged (4), reasons documented, fixes belong to mtl_replay.m.
