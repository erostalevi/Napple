# Scanning a title for GPU stages (`tools/gpu/scan_stages.py`)

Tells you, without playing, which GPU features a Switch title's shader programs use (geometry, tessellation control/eval, transform feedback, compute, tessellation domains), and picks the smallest set of titles that covers every feature. Python 3 standard library only.

## Run it on a new game
1. Build the title: `napple build <container> --keys prod.keys` (see [add-game-support](add-game-support.md)).
2. Either dump what the title really creates, or scan its RomFS (or both):
   - Runtime dump (needs a Mac): `NAPPLE_AUDIO_DEVICE=0 NAPPLE_GFX_CTL_DUMP=/tmp/ctl-<title> <run the title ~45 s>` (create the directory first). Writes `s<stage>-<hash>.ctl` and `.code` per program.
   - RomFS: `napple build ... --dump-romfs` and extract the shader archives (`.bfsha`, `.szs`) into a directory.
3. Scan: `python3 -I tools/gpu/scan_stages.py scan /tmp/ctl-<title> --title-id 0100XXXXXXXXXXXX -o scans/<title>.json` (several inputs: directories and RomFS images, see below). A directory is walked: `*.ctl` files are decoded directly; any other file (Yaz0 `.szs` is inflated first) is searched for control sections (magic `0x98761234`, 4-aligned, validated like `gfx_control_parse`). Printed JSON: `{title_id, programs, stages:{vs,fs,gs,tcs,tes,cs,xfb,tess_*}, features, not_decodable}`; a short summary goes to stderr.
4. Choose what to buy/test: `python3 -I tools/gpu/scan_stages.py cover scans/*.json` prints the exact smallest set (up to 20 titles; greedy otherwise) and the features no scanned title covers.

## Limits
- Offsets come from `gfx/src/shader/control.h` / `control.c` at run time (`offsets_from` in the output). The transform feedback and tessellation-mode offsets are still a hypothesis there; `xfb` / `tess_*` are counted only when the bytes pass the same self-consistency checks as the runtime readers.
- MSAA / sample shading and sparse/tiled resources are created by NVN API calls, not by a field known in the control section, so they are listed under `not_decodable` and are not in the cover. Use runtime logs for those.
- Compressed archives other than Yaz0 (zstd `.zs`) are not inflated; decompress them first.
- Tests: `python3 -I tools/gpu/test_scan_stages.py` (synthetic sections only).

Reference results from b7 to compare against on the Mac: Super Mario Odyssey 6 geometry programs (3 distinct), Animal Crossing 4 (2 distinct), Mario Kart 8 Deluxe 1, Link's Awakening 0.

## RomFS images (base, update, DLC) and real programs (2026-11-01)
- RomFS: `napple build --dump-romfs <base.nsp> -k prod.keys -o base.romfs` for a base; for an update, `napple build <base> <update> -k prod.keys --updates latest --dlc none --update-romfs materialize --data-only -o <dir>` writes the patched `romfs.bin` per version; DLC `romfs.bin` files sit in a data build's `dlc/<id>/v<n>/`. Then `python3.14 -I tools/gpu/scan_stages.py scan base.romfs <dir>/<titleid>/<build>/romfs.bin dlc/*/v0/romfs.bin --title-id <id> -o scans/<title>.json`. SARC archives and Yaz0 are opened (model archives carry shader programs: Mario Kart's course and driver `.szs`), zstd needs Python 3.14 (`compression.zstd`); `--quick` opens only shader-looking paths. A full title takes 2-10 minutes (pure-Python Yaz0); `distinct` counts sections by content.
- Real programs through the replay: `NAPPLE_GFX_STAGE_DUMP=<a NAPPLE_GFX_CTL_DUMP dir> MTL_DEBUG_LAYER=1 gfx/build/tests/t_stages_dumped` runs every dumped geometry program with points / lines / triangles input and checks the captured output (`T_VERBOSE=1` prints the vertices). The code is game data: keep the dump outside the repository.

| Title (versions scanned) | Programs (distinct vs / fs / cs) | Geometry | Tess control / eval | Transform feedback | Where the geometry programs are |
|---|---|---|---|---|---|
| Link's Awakening 0100…6BB00C6F0000 (v0) | 6429 (2881 / 2858 / 26) | 0 | 0 | 0 | - |
| Super Mario Odyssey 0100000000010000 (v0) | 13910 (5045 / 8107 / 68) | 2 distinct (line strip, 64 and 2 vertices) | 0 | 0 | `ShaderData/alThunderShader.szs`; stage data referencing thunder (inferred, not seen drawn): the Odyssey travel cutscenes (`DemoWorldMove*StageDesign` carries `RequestThunderVtxNum`, `DistantDemoWorldMoveThunder`), New Donk City, the Ruined Kingdom, the Moon Bowser stages; 3 hashes created at boot |
| Mario Kart 8 Deluxe 0100152000022000 (v1376256 patched + Booster DLC) | 36275 (10402 / 13239 / 24) | 1 (code 0x300) | 0 | 0 | `Common/Gfx/gsys_resource.Nin_NX_NVN.release.sarc`; not created in boot runs |
| Animal Crossing 01006F8002326000 (3.0.3 patched + 3 DLC) | 9720 (2533 / 3505 / 56) | 8 distinct (triangle strip, 6 vertices) | 0 | 0 | `Swkbd/shader/*OceanShader*` (the game's own keyboard; NX and Win variants); 2 created at boot, the name-entry keyboard does not bind them |

Cover: `{gs}` from any of SMO / MK8D / ACNH; tess control, tess eval, isolines / triangle / quad domains and transform feedback: no title on hand.

## Shader corpus: every program of a title through the translator and Metal (2026-11-01, r2)
1. Extract (game data: keep it outside the repository, e.g. `/Users/neto/NappleWork/shader-corpus/<title>`): `python3.14 -I tools/gpu/scan_stages.py extract <romfs images and NAPPLE_GFX_CTL_DUMP dirs> -o <corpus dir> --tag <title>`. Each distinct program is written as `<tag>_<digest>_<vs|ps|gs|tcs|tes|cs>.bin` (code area) + `.ctl`. Code is paired with its control section through the BNSH program record (u64 control offset, u64 code offset, relative to the `BNSH` header: bfsha, VFXB effects, bfres); agl `BAHS` archives (Odyssey's ShaderData and AglResource, ACNH / MK8D agl resources) have no readable record, so their code areas are written alone (stage from the SPH, cut after GLSLC's terminal backward branch); compute code areas there have no header and are reported as sections without code. The JSON on stdout counts paired / covered / unpaired sections (`unpaired_by_file`).
2. Translate and compile: `NAPPLE_SHADER_CORPUS=<corpus dir> gfx/build/tests/t_xlate` (add `NAPPLE_SHADER_CORPUS_LIST=1` to name every failing file). Per tag: programs, translated, compiled with Metal (MSL 3.1 when the program needs it, 3 compiles at a time), failures grouped by message. A translate failure is a translator gap; a Metal failure is a translator bug in the emitted text.
3. A title's whole RomFS takes 5-15 minutes to extract and 5-35 minutes to compile.
