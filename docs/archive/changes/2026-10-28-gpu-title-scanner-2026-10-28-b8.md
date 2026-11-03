# gpu-title-scanner (b8)

New `tools/gpu/scan_stages.py`: `scan` decodes GLSLC control sections from a `NAPPLE_GFX_CTL_DUMP` directory or finds them inside RomFS shader archives (Yaz0 inflated) and reports per-title JSON `{title_id, programs, stages, features}`; `cover` runs greedy plus exact set cover over several scan files and lists features no title covers. Offsets are read from `gfx/src/shader/control.h/.c` at run time with fallbacks. MSAA and sparse are reported as not decodable (no known control-section field).

Status: tested on synthetic sections (`python3 -I tools/gpu/test_scan_stages.py`, 5 tests), not verified on real titles. Mac check: LAS/MK8D/ACNH/SMO dumps vs the b7 findings (SMO 6 gs programs, ACNH 4, MK8D 1).

Files: `tools/gpu/*`, `docs/guides/gpu-title-scan.md`, one row in `tools/README.md`.
