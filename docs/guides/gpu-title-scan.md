# Scanning a title for GPU stages (`tools/gpu/scan_stages.py`)

Tells you, without playing, which GPU features a Switch title's shader programs use (geometry, tessellation control/eval, transform feedback, compute, tessellation domains), and picks the smallest set of titles that covers every feature. It also extracts every program of a title into a shader corpus for the translator. Python 3 standard library only (zstd archives need Python 3.14).

Game data stays outside the repository: control sections, code areas and corpora are extracted from games you own; keep them in a work directory such as `<work dir>/shader-corpus/<title>` and never commit them.

## Run it on a new title
1. Build the title: `napple build <container> --keys prod.keys` (see [add-game-support](add-game-support.md)).
2. Either dump what the title really creates, or scan its RomFS (or both):
   - Runtime dump (needs a Mac): `NAPPLE_AUDIO_DEVICE=0 NAPPLE_GFX_CTL_DUMP=<work dir>/ctl-<title> <run the title ~45 s>` (create the directory first). Writes `s<stage>-<hash>.ctl` and `.code` per program.
   - RomFS: `napple build ... --dump-romfs` and extract the shader archives (`.bfsha`, `.szs`) into a directory, or pass RomFS images directly (next section).
3. Scan: `python3 -I tools/gpu/scan_stages.py scan <work dir>/ctl-<title> --title-id 0100XXXXXXXXXXXX -o scans/<title>.json` (several inputs: directories and RomFS images). A directory is walked: `*.ctl` files are decoded directly; any other file (Yaz0 `.szs` is inflated first) is searched for control sections (magic `0x98761234`, 4-aligned, validated like `gfx_control_parse`). Printed JSON: `{title_id, programs, stages:{vs,fs,gs,tcs,tes,cs,xfb,tess_*}, features, not_decodable}`; a short summary goes to stderr. `distinct` counts sections by content.
4. Choose what to test: `python3 -I tools/gpu/scan_stages.py cover scans/*.json` prints the exact smallest set (up to 20 titles; greedy otherwise) and the features no scanned title covers.

## RomFS images (base, update, DLC)
- Base: `napple build --dump-romfs <base.nsp> -k prod.keys -o base.romfs`.
- Update: `napple build <base> <update> -k prod.keys --updates latest --dlc none --update-romfs materialize --data-only -o <dir>` writes the patched `romfs.bin` per version.
- DLC: the `romfs.bin` files sit in a data build's `dlc/<id>/v<n>/`.
- Then `python3.14 -I tools/gpu/scan_stages.py scan base.romfs <dir>/<titleid>/<build>/romfs.bin dlc/*/v0/romfs.bin --title-id <id> -o scans/<title>.json`.

SARC archives and Yaz0 are opened (model archives such as course or character `.szs` files can carry shader programs); zstd needs Python 3.14 (`compression.zstd`); `--quick` opens only shader-looking paths. A full title takes 2-10 minutes (pure-Python Yaz0).

## Running real geometry programs through the replay
`NAPPLE_GFX_STAGE_DUMP=<a NAPPLE_GFX_CTL_DUMP dir> MTL_DEBUG_LAYER=1 gfx/build/tests/t_stages_dumped` runs every dumped geometry program with points / lines / triangles input (constant buffers of 1.0) and checks the captured output (`T_VERBOSE=1` prints the vertices). Skipped without the variable.

## Shader corpus: every program of a title through the translator and Metal
1. Extract: `python3.14 -I tools/gpu/scan_stages.py extract <romfs images and NAPPLE_GFX_CTL_DUMP dirs> -o <corpus dir> --tag <title>`. Each distinct program is written as `<tag>_<digest>_<vs|ps|gs|tcs|tes|cs>.bin` (code area) + `.ctl`. Code is paired with its control section through the BNSH program record (u64 control offset, u64 code offset, relative to the `BNSH` header: bfsha, VFXB effects, bfres). agl `BAHS` archives have no readable record, so their code areas are written alone (stage from the SPH, cut after GLSLC's terminal backward branch); compute code areas there have no header and are reported as sections without code. The JSON on stdout counts paired / covered / unpaired sections (`unpaired_by_file`).
2. Translate and compile: `NAPPLE_SHADER_CORPUS=<corpus dir> gfx/build/tests/t_xlate` (add `NAPPLE_SHADER_CORPUS_LIST=1` to name every failing file, `NAPPLE_SHADER_CORPUS_XLATE_ONLY=1` to skip the Metal compiles). Per tag: programs, translated, compiled with Metal (MSL 3.1 when the program needs it, 3 compiles at a time), failures grouped by message, and how many programs write the viewport index or the layer. A translate failure is a translator gap; a Metal failure is a translator bug in the emitted text.
3. A title's whole RomFS takes 5-15 minutes to extract and 5-35 minutes to compile.

## Limits
- Offsets come from `gfx/src/shader/control.h` / `control.c` at run time (`offsets_from` in the output). The transform feedback and tessellation-mode offsets are still a hypothesis there; `xfb` / `tess_*` are counted only when the bytes pass the same self-consistency checks as the runtime readers.
- MSAA / sample shading and sparse/tiled resources are created by NVN API calls, not by a field known in the control section, so they are listed under `not_decodable` and are not in the cover. Use runtime logs for those.
- Tests: `python3 -I tools/gpu/test_scan_stages.py` (synthetic sections only).

Results of the scans of the reference titles are in the [title notes](../titles/README.md).
