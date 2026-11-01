# gfx-shader-views (2026-11-01, batch r2): the four titles' shader corpus through the translator and Metal, integer sampled textures, the ACNH cube view, sparse remaps, the MK8D binary archive

Branch `claude/gfx-shader-views-2026-11-01-r2` (Mac, Metal, base 79415f0). Contract: [gfx.md section 13](../../architecture/gfx.md) ("Shader corpus, integer textures, cube and sparse views"), [gpu-title-scan](../../guides/gpu-title-scan.md) ("Shader corpus").

## What changed
1. **Corpus** (`tools/gpu/scan_stages.py extract`, `gfx/tests/t_xlate.m` corpus mode). Extract writes every distinct program of a title (RomFS images and `NAPPLE_GFX_CTL_DUMP` dirs) as `<tag>_<digest>_<stage>.bin/.ctl`, outside the repository. Code is paired with its control section through the BNSH program record (u64 control offset + u64 code offset relative to the `BNSH` header; bfsha and VFXB). agl `BAHS` archives have no readable record, so their code areas are written alone (stage from the SPH, cut after GLSLC's terminal backward branch). The `t_xlate` corpus mode now handles every stage (geometry and tess keys from the SPH), uses MSL 3.1 when needed, runs 3 compiles at a time, and reports per-title totals with failures grouped by message.
2. **Translator fixes found by the corpus** (`TRANSLATOR_VERSION` 16):
   - `LEA.HI` constant-buffer form (opcode 000110, yuzu's layout; SMO compute programs).
   - `TEX` AOFFI: `int3` on 3D textures, none on 1D and cube.
   - `TLDS` texture_info 6 (2D.LZ.MS).
   - `TXQ` / `TXA` through `txq_*` overloads, so a later `TLDS.MS` can make the binding multisampled.
   - `TLD4.PTP`: four gathers, the base texel of the gather at offset i.
3. **Integer sampled textures**:
   - `ShaderKey.tex_uint_slots` / `tex_sint_slots` declare a binding `texture2d<uint|int>`, read through the `IntTex2D` / `IntTex2DArray` wrappers: four channels as raw integer bits, sign-extended for signed formats.
   - `pipe_key.tex_uint/tex_sint` carry the masks, and `mtl_shader.m` copies them into the per-stage keys.
   - `mtl_tex_int_kind` gives a binding's integer kind; once the replay asks for it, integer views of another class are copies.
   - The masks are filled by `int_slot_masks` in `mtl_replay.m` / `mtl_stages.inc` (gfx-nvn-api's files this round). That hook is in the integrator file and not committed here.
4. **Cube view of a 2D texture** (`mtl_tex.m` `AK_CUBE`): a copy-backed cube with the 2D image on every face.
5. **Sparse residency**:
   - `nvnMemoryPoolMapVirtual` runs in `mtl_tex_remap_scope`, so textures of the remapped range are kept instead of being forgotten and recreated without the GPU-written tiles.
   - Sparse textures never become `gpu_owned`.
   - A residency change re-uploads only the 64 KiB tiles whose pages changed (`upload_texture_part`).
6. **Binary archive** (`mtl_shader.m`):
   - Root cause of MK8D's "binary archive save failed ... renaming": Metal's `serializeToURL:` answers YES and writes nothing when the archive gained nothing since its last serialization (for example, re-adding a pipeline it already holds), so the rename found no file. That case now means "the file is current".
   - Temporary names are per process; stale ones of dead processes are removed.
   - A real rename failure logs errno and retries at the next save.

## Verified vs not
| Item | Status |
|---|---|
| Corpus: LAS 5767, ACNH 8577, MK8D 26544, SMO 31750 programs; all translate and compile with Metal | verified (real programs, this Mac) |
| `make -C gfx test` (incl. `t_cube_view`, `t_sparse`, `t_xlate` pins; `t_int_tex` skips without the hook) | tested |
| `t_int_tex` with the replay hook applied locally | tested (fails without the hook: Metal validation, uint texture bound to a float parameter) |
| ACNH cube view: no "not expressible" and no "dummy texture" lines, the cube alias in use | verified (ACNH 90 s, base vs head) |
| Sparse remap re-upload, GPU-written tiles kept | tested (`t_sparse`; no title on hand uses sparse textures) |
| MK8D archive rename failure | root cause reproduced standalone; the MK8D 110 s race run that failed in r1 saves the archive with no failure (verified) |
| Four-title boots | verified: no regression (see the integrator file) |
