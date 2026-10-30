# Scanning a title for GPU stages (`tools/gpu/scan_stages.py`)

Tells you, without playing, which GPU features a Switch title's shader programs use (geometry, tessellation control/eval, transform feedback, compute, tessellation domains), and picks the smallest set of titles that covers every feature. Python 3 standard library only.

## Run it on a new game
1. Build the title: `napple build <container> --keys prod.keys` (see [add-game-support](add-game-support.md)).
2. Either dump what the title really creates, or scan its RomFS (or both):
   - Runtime dump (needs a Mac; use the game lock): `NAPPLE_AUDIO_DEVICE=0 NAPPLE_GFX_CTL_DUMP=/tmp/ctl-<title> python3 tools/dev/gamelock.py -- <run the title ~45 s>` (create the directory first). Writes `s<stage>-<hash>.ctl` and `.code` per program.
   - RomFS: `napple build ... --dump-romfs` and extract the shader archives (`.bfsha`, `.szs`) into a directory.
3. Scan: `python3 -I tools/gpu/scan_stages.py scan /tmp/ctl-<title> --title-id 0100XXXXXXXXXXXX -o scans/<title>.json`. A directory is walked: `*.ctl` files are decoded directly; any other file (Yaz0 `.szs` is inflated first) is searched for control sections (magic `0x98761234`, 4-aligned, validated like `gfx_control_parse`). Printed JSON: `{title_id, programs, stages:{vs,fs,gs,tcs,tes,cs,xfb,tess_*}, features, not_decodable}`; a short summary goes to stderr.
4. Choose what to buy/test: `python3 -I tools/gpu/scan_stages.py cover scans/*.json` prints the exact smallest set (up to 20 titles; greedy otherwise) and the features no scanned title covers.

## Limits
- Offsets come from `gfx/src/shader/control.h` / `control.c` at run time (`offsets_from` in the output). The transform feedback and tessellation-mode offsets are still a hypothesis there; `xfb` / `tess_*` are counted only when the bytes pass the same self-consistency checks as the runtime readers.
- MSAA / sample shading and sparse/tiled resources are created by NVN API calls, not by a field known in the control section, so they are listed under `not_decodable` and are not in the cover. Use runtime logs for those.
- Compressed archives other than Yaz0 (zstd `.zs`) are not inflated; decompress them first.
- Tests: `python3 -I tools/gpu/test_scan_stages.py` (synthetic sections only).

Reference results from b7 to compare against on the Mac: Super Mario Odyssey 6 geometry programs (3 distinct), Animal Crossing 4 (2 distinct), Mario Kart 8 Deluxe 1, Link's Awakening 0.
