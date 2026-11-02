---
branch: claude/gfx-shader-views-2026-11-01-r2
orchestration: gfx-shader-views
base: 79415f0
head: c7ad49d  # last work commit; the integrator-file commit sits on top
env: mac
date: 2026-11-01
areas: [gfx/shader, gfx/tex, gfx/memory, tools/gpu, docs]
status: ready-with-caveats
---

# claude/gfx-shader-views-2026-11-01-r2: full shader corpus of the four titles, integer textures, ACNH cube view, sparse remap, MK8D archive

## Summary
- **Corpus (acceptance 1)**: `scan_stages.py extract` writes every distinct program of a title outside the repo (`/Users/neto/NappleWork/shader-corpus/<title>`). `t_xlate`'s corpus mode runs them all, every stage included. Final run: **LAS 5767, ACNH 8577, MK8D 26544, SMO 31750 programs (72638); all translate, all compile with Metal**. Five translator gaps the corpus hit are fixed: `LEA.HI` cbuf, `TEX` 3D AOFFI, `TLDS` 2D.LZ.MS, `TXQ` of multisampled bindings, `TLD4.PTP`.
- **Integer sampled textures (acceptance 2)**: done on the translator, key and `mtl_tex.m` side. The slot masks come from a **10-line hook in `mtl_replay.m` + 1 line in `mtl_stages.inc`**; gfx-nvn-api owns both files this round, so the hook isn't committed (patch below). Tested locally with the hook (`t_int_tex`); until it lands, behaviour is unchanged.
- **ACNH cube view (acceptance 3)**: verified. A cube view of a 2D texture is now a copy-backed cube; ACNH's "not expressible" and "using a dummy texture" lines are gone.
- **Sparse (acceptance 4)**: residency changes after upload re-upload only the changed tiles, and GPU-written tiles survive (`t_sparse`). Two causes were found and fixed: textures of a remapped range were forgotten and recreated, and a GPU copy made the whole sparse texture `gpu_owned`, so later residency changes were ignored.
- **MK8D archive (acceptance 5)**: root cause found and reproduced standalone. Metal's `serializeToURL:` answers YES and writes no file when the archive gained nothing since its last serialization. Fixed, and temporary names are now per process. The MK8D 110 s race run that failed in r1 (at about 78 s) now saves the archive (1223 pipelines) with no failure line.
- **Boots (acceptance 6)**: no regression in base vs head for the four titles.

## Files
`git diff --stat 79415f0..HEAD`:
- `gfx/src/shader/maxwell_translate.c` (fixes + integer textures), `gfx/src/shader/maxwell.h` (append: `ShaderKey.tex_uint_slots` / `tex_sint_slots`, `ShaderTexture.int_kind`)
- `gfx/src/mtl_shader.m` (key copies, `TRANSLATOR_VERSION` 16, archive save)
- `gfx/src/mtl_tex.m` (`mtl_tex_int_kind`, `AK_CUBE`, sparse partial upload, `mtl_tex_remap_scope`, not-`gpu_owned` sparse)
- `gfx/src/mtl_priv.h` (`pipe_key.tex_uint/tex_sint`, prototype)
- `gfx/src/api_memory.c` (remap scope)
- Tests: `gfx/tests/t_int_tex.c` (new), `gfx/tests/t_cube_view.c` (new), `gfx/tests/t_sparse.c`, `gfx/tests/t_xlate.m` (corpus mode + pins), `gfx/tests/t_xlate_stages.inc`
- `tools/gpu/scan_stages.py`, `tools/gpu/test_scan_stages.py`
- Shared docs, append-only: `docs/architecture/gfx.md` (one paragraph in section 13), `docs/guides/gpu-title-scan.md` (new section)
- `docs/archive/changes/2026-11-01-claude-gfx-shader-views-2026-11-01-r2.md` (new)
- Not touched: `mtl_replay.m`, `mtl_stages.inc`, `api_*.c` (other than `api_memory.c`), `core.c`, `nvnapi/`, `mtl_present.m`, `runtime/`, `orchestrations/`. The `gfx/Makefile` test list needed no change (it uses a wildcard).

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| 1. Corpus of the four titles (base + updates + DLC) | **verified** | `NAPPLE_SHADER_CORPUS=/Users/neto/NappleWork/shader-corpus/<t> gfx/build/tests/t_xlate` -> `corpus las: 5767 programs, 5767 translated, 5767 compile with Metal`, `acnh: 8577 / 8577 / 8577`, `mk8d: 26544 / 26544 / 26544`, `smo: 31750 / 31750 / 31750` (logs `/Users/neto/NappleWork/shader-corpus/*-xlate-final.txt`). Before the fixes: SMO 61 translate + 8 Metal failures, MK8D 4 + 8. |
| 1. Sections without code | documented | ACNH 31 (compute in agl archives, 4 in `System/Font/nvn_font_shader*.bin`, 2 in gsys), SMO 33 (compute in agl archives), MK8D 0, LAS 0. These are compute programs (no header to find) or font binaries without a BNSH record. |
| 2. Integer sampled textures | tested with the hook applied locally | hook (patch below) + `make -C gfx build/tests/t_int_tex EXTRA_CFLAGS=-DNAPPLE_TEST_INT_TEX_SLOTS && MTL_DEBUG_LAYER=1 gfx/build/tests/t_int_tex` -> `RGBA8UI view: 1 2 3 4`, `RGBA8I view: ffffffff ffffff80 7f 1`, `RG32UI view of BC1: 8410f800 e4e4e4e4 0 1`, `ok`. Without the hook the same test aborts: `MTLPixelFormatRGBA8Uint ... incompatible with the data type (MTLDataTypeFloat)`. In the branch as committed it is `ok (1 skipped)`. |
| 3. ACNH cube view | **verified** | ACNH 90 s (A at 25 / 40 / 55 / 70 s, debug layer, private save copy), base 79415f0 vs head. Base: `texture view type 5 of a type 2 texture is not expressible` + `texture slot 0: the shader samples texture type 5 but type 2 is bound: using a dummy texture`. Head: neither line; `copy-backed texture views in use (first: kind 4, format 10 -> 10, type 2 -> 5, levels 0+4 slices 0+6)`. The texture is 15x15 R8, 4 levels, 2560 bytes (a cube needs 18432). Frames 1000 / 2000 match base (mean difference 1.4 / 1.1 of 255). `t_cube_view` checks six face directions (zeros with the old code). |
| 4. Sparse remap | tested | `t_sparse`: after a remap, tile (0,0) reads its new pages and the GPU-copied tile (1,0) is kept; after an unmap, tile (0,0) reads zero. Failed before (whole re-upload wiped tile (1,0), then later changes were ignored). No title on hand creates sparse textures. |
| 5. MK8D archive rename | verified (no failure in the in-game run; the cause is intermittent, so the standalone reproduction is the proof of mechanism) | Standalone reproduction: an `MTLBinaryArchive` serialized twice with nothing new (or with an already-held pipeline added again) gets `serialize 1, file written 0, rename No such file or directory`. Fix: no file written means the archive on disk is current. In game: MK8D 110 s with A every 1.5 s from 20 s (reaches a Grand Prix race; same script as r1's `gfxstages-mk8d-race`, which logged `binary archive save failed, archive disabled for this run: renaming the saved archive into place failed` at 78 s): exit 0, no failure line, `binary archive: 136 of 1223 pipelines loaded precompiled this run`, `pipelines-v16.metalar` 16 MB written at the end (log `/Users/neto/NappleWork/logs/gfxviews-mk8d-arch/run.log`). |
| 6. Four-title boots | **verified: no regression** | base 79415f0 vs head, 60 s (ACNH 90 s), debug layer, cold private save copies: all 8 exit 0 with 0 skipped draws; warning sets identical except ACNH's removed dummy-texture warning; no Metal validation lines. Frames base -> head: ACNH 2677 -> 2676, LAS 3409 -> 3549, SMO 2363 -> 2394, MK8D 3553 -> 3550. Screenshot differences are timing (SMO f-2000 6.4, LAS f-1000 22: the animated intro storm at a different moment). Logs `/Users/neto/NappleWork/logs/gfxviews-reg-{base,head}-<titleid>/`. |
| `make -C gfx test` | tested | ALL TESTS PASSED (t_cube_view, t_sparse, t_xlate, t_int_tex skip) |
| `python3 -I tools/gpu/test_scan_stages.py` | tested | OK (7 tests; also python3.14) |

## Unbuilt / untested list
- Nothing unbuilt.
- `int_slot_masks` (hook) is not in the branch; `t_int_tex` is gated by `-DNAPPLE_TEST_INT_TEX_SLOTS`.
- Integer textures are unverified in a game: none of the four titles' scenes reached here samples an integer view (no `texture view format ... not expressible` line for an integer format in the boots).
- `TLD4.PTP` and `TLDS.MS` are compile-tested (corpus + `t_xlate`); their results aren't checked against hardware.

## Findings
- **Pairing code with control sections.** BNSH records carry u64 control offset + u64 code offset relative to the `BNSH` header; the size fields after them differ between bfsha and VFXB. Control sections are not unique per program (SMO's 14910 runtime programs share about 6480 sections). agl `BAHS` archives share code areas between programs, so their code areas are written as programs of their own. GLSLC ends a program with a backward BRA to itself (`e2400fffff87000f`) or to the final EXIT (`e2400fffff07000f`). Compute code areas have no header (instructions from the first word).
- **Translator decisions**:
  - `LEA.HI` cbuf uses yuzu's `LEA_HI_C` layout, confirmed by real words. The register form of `.HI` in the translator still uses the from-memory layout (neg at 45 applied to B, X at 48). yuzu has neg at 37 (negating the 64-bit offset) and X at 38. No real register-form word appeared in 72638 programs, so it's left as is, pinned by `t_xlate`.
  - `TLD4.PTP` follows SPIRV-Cross (component i = `.w` of the gather at offset i).
  - `TLDS` info 6 follows yuzu (2D.LZ.MS).
- **Integer textures**: the old float declaration was a Metal validation error with the debug layer and undefined without it, for any texture or view in an integer colour format. Integer views of 3D / cube / MS targets keep the float declaration (only 2D and 2D array are wrapped).
- **Cube of a 2D texture**: the console reads unrelated memory for faces 1-5; replicating face 0 is a choice, documented in `mtl_tex.m`.
- **Sparse**: `gfx_mtl_pool_remove` (mtl_core.m) forgets textures by address, which is right for a finalized pool and wrong for a remap. The remap scope is thread-local around `vmap_cut` in `api_memory.c`.
- **Shader cache files** (`cache_store`) used the same single `.tmp` name as the archive; they now use per-process names too.
- **Outside my area (not fixed)**: an MK8D A32 run crashed once (SIGBUS in `a32jit_fb`, guest fault at 0x7000000028, at about 85 s, log `/Users/neto/NappleWork/logs/gfxstages-mk8d-arch/run.log`). It didn't repeat in the 60 s boots or the final 110 s race run.
- `tools/boot_test.sh` writes to the shared `work/saves`; all my runs used private save copies (`/Users/neto/NappleWork/saves-gfxstages*`, `saves-r2-*`), which can be deleted.

## How to integrate
- **Hook (needs gfx-nvn-api's files; apply after both branches merge):**
  ```diff
  --- a/gfx/src/mtl_replay.m
  +++ b/gfx/src/mtl_replay.m
  @@ static void ds_slot_masks(...) { ... }
  +/* Sampled bindings of a stage whose bound texture / view is an integer colour format (ShaderKey.tex_uint_slots / tex_sint_slots: declared texture2d<uint|int>). */
  +static void int_slot_masks(int stage, uint32_t *umask, uint32_t *smask) {
  +    *umask = *smask = 0;
  +    gdpool *tp = (gdpool *)htab_get(S.texpool);
  +    if (!tp) return;
  +    for (int slot = 0; slot < 32; slot++) {
  +        uint64_t h = S.tex[stage][slot];
  +        uint32_t tic = (uint32_t)(h & 0xfffff);
  +        if (!h || tic >= (uint32_t)tp->n || !tp->tex[tic].s.valid) continue;
  +        int k = mtl_tex_int_kind(&tp->tex[tic].s, &tp->tex[tic].view, 1);
  +        if (k == IMGKIND_UINT) *umask |= 1u << slot;
  +        else if (k == IMGKIND_INT) *smask |= 1u << slot;
  +    }
  +}
  +
   static void fill_pipe_key(uint32_t prim, pipe_key *k) {
       memset(k, 0, sizeof *k);
  +    int_slot_masks(ST_VS, &k->tex_uint[0], &k->tex_sint[0]);
  +    int_slot_masks(ST_FS, &k->tex_uint[1], &k->tex_sint[1]);
  --- a/gfx/src/mtl_stages.inc   (stage_key_masks)
  +    int_slot_masks(stage, &sk->tex_uint_slots, &sk->tex_sint_slots);
  ```
  Then remove the `#ifdef NAPPLE_TEST_INT_TEX_SLOTS` / `#else` stub in `gfx/tests/t_int_tex.c`.
- `TRANSLATOR_VERSION` is 16 here. If gfx-nvn-api also changes generated MSL, take the higher number or bump once more.
- Conflict hotspots: `gfx/src/mtl_priv.h` (`pipe_key` fields appended at the end), `docs/architecture/gfx.md` section 13 (keep both paragraphs).
- Build: `make -C gfx test` -> ALL TESTS PASSED; `python3 -I tools/gpu/test_scan_stages.py` -> OK; `make -C runtime`.

## How to test
1. `make -C gfx test`: ALL TESTS PASSED; `t_cube_view: ok`, `t_sparse: ok`, `t_xlate: ok`, `t_int_tex: ok (1 skipped)`. After the hook: rebuild `t_int_tex` without the guard -> `ok (0 skipped)`.
2. Corpus (the corpora already exist on this Mac): `NAPPLE_SHADER_CORPUS=/Users/neto/NappleWork/shader-corpus/smo gfx/build/tests/t_xlate` -> `corpus smo: 31750 programs, 31750 translated, 31750 compile with Metal` (about 25 min; `las` about 4 min). To rebuild a corpus: `docs/guides/gpu-title-scan.md`.
3. ACNH 90 s (A at 25 / 40 / 55 / 70 s) -> `copy-backed texture views in use (first: kind 4, ... type 2 -> 5 ...)`, and no `not expressible` / `dummy texture` lines.
4. MK8D 110 s with A every 1.5 s from 20 s (reaches a race) -> no `binary archive save failed`; `pipelines-v16.metalar` updated in the save root's cache directory.
5. Regression guard: the four 60 s boots (identical warning sets to base, 0 skipped draws, no validation lines).

## Fix policy hints
- Safe for the integrator: `TRANSLATOR_VERSION` collisions, removing the test guard once the hook is in, `pipe_key` field order on conflict (append both).
- Needs a decision: whether a cube view of a 2D texture should replicate face 0 (done here) or read zeros for faces 1-5. Also the `LEA.HI` register-form layout (from memory vs yuzu): only change it with a real word.

## Needs from other areas
- gfx-nvn-api (`mtl_replay.m`, `mtl_stages.inc`): the `int_slot_masks` hook above.
