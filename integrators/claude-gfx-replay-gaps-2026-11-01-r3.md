---
branch: claude/gfx-replay-gaps-2026-11-01-r3
orchestration: gfx-replay-gaps
base: 84cb5a6
head: 4c74e0f  # last work commit; the integrator-file commit sits on top
env: mac
date: 2026-11-01
areas: [gfx/replay, gfx/shader, docs]
status: ready-with-caveats
---

# claude/gfx-replay-gaps-2026-11-01-r3: D24 readback, separate texture/sampler binds, integer 3D/cube/MS views

## Summary
- **D24 / D24S8 readback (acceptance 1):** texture -> buffer copies of DEPTH24 (0x33) / DEPTH24_STENCIL8 (0x35) now write NVN's packed words, the exact inverse of `copy_d24_to_tex` (`copy_d24_from_tex`: blit to staging + a pack kernel). The "reading back a 24-bit depth texture ... is not implemented" warning is gone. Stencil sampling of cube views (optional) is not done.
- **Separate texture / sampler binds (acceptance 2):** the replay keeps `BindSeparateTexture(s)` / `BindSeparateSampler(s)` and their `Deferred` forms per stage. The translator turns a bindless TEX whose handle is (cbuf word) | (cbuf word) (LOP.OR, IADD, or LOP3 with an OR table) into a separate pair. `bind_stage` ORs the two words, read from c2's tables or a user uniform buffer. The "separate texture/sampler binding is not supported yet" warning is gone.
  - **Caveat:** where GLSLC places the separate tables in c2 is a hypothesis (`SEP_TEX_C2_BASE` 0x120, `SEP_SAMP_C2_BASE` 0x520, 8 bytes per binding, `gfx/src/shader/maxwell.h`). No title on hand binds separate handles, so it can't be confirmed. Handles read from a user uniform buffer need no layout. The replay logs the offsets a shader uses once, for calibration.
- **Integer views of 3D / cube / MS targets (acceptance 3):** declared as `texture3d` / `texturecube` / `texture2d_ms<uint|int>` and read through `IntTex3D` / `IntTexCube` / `IntTex2DMS`. Before, they kept the float declaration (a Metal validation error).
- **Regression (acceptance 4):** `make -C gfx test` ALL TESTS PASSED; LAS corpus 5767 / 5767 / 5767; the four-title boots show no regression.

## Files
`git diff --stat 84cb5a6..HEAD`:
- `gfx/src/mtl_replay.m`: `copy_d24_from_tex`, `handle_word`, the separate binding state and ops, separate textures in `bind_stage`.
- `gfx/src/shader/maxwell_translate.c`: pair detection (`pair_detect`, `reg_cb2`), `separate_index` / `handle_index`, separate `tex_slot`, the integer wrappers.
- `gfx/src/shader/maxwell.h`: `ShaderTexture.separate` / `sep_cbuf` / `sep_offset`, the `SEP_*_C2_*` constants, the `tex_*_slots` comment.
- `gfx/src/mtl_shader.m`: `TRANSLATOR_VERSION` 17.
- Tests: `gfx/tests/t_d24_readback.c` (new), `gfx/tests/t_sep_sampler.c` (new), `gfx/tests/t_int_tex.c` (cases 4-6), `gfx/tests/t_xlate.m` (pins).
- Docs: `docs/architecture/gfx.md` (one paragraph in section 13), `docs/archive/changes/2026-11-01-claude-gfx-replay-gaps-2026-11-01-r3.md`.
- Not touched: `obj.h`, `api_*.c`, `nvnapi/`, `runtime/`, `orchestrations/`. No API-side change was needed: the separate binds already reach the replay as `OP_BIND_SEPTEX` / `OP_BIND_SEPSAMP`.

## Built and tested here
| Item | Status | Evidence |
|---|---|---|
| 1. D24 / D24S8 readback | tested | `MTL_DEBUG_LAYER=1 gfx/build/tests/t_d24_readback` -> `DEPTH24: 0 of 64 words differ`, `DEPTH24_STENCIL8: 0 of 64 words differ` (values 0, 1, 0x800000, 0xffffff, 0x123456, 0xabcdef + a ramp; every stencil pattern), `ok`. No title read one back in these boots (no warning line in any log before either). |
| 2. Separate texture / sampler | tested | `t_sep_sampler`: c2 words with LOP.OR, clamp sampler -> texel 0 `ff0000ff`, repeat sampler -> texel 3 `ffffffff`; uniform buffer words with IADD -> `ff0000ff`; log `separate texture / sampler handles in use (first: c2[0x120] \| c2[0x520] -> handle 0x70012c; c2 tables ... are a hypothesis)`. Before this branch the translator refused the program ("bindless texture handle is not a constant buffer load"). `t_xlate` pins the pair. |
| 3. Integer 3D / cube / MS | tested | `t_int_tex` (debug layer, no validation error): `3D RGBA8UI: 5 6 7 8`, `cube view of 2D RGBA8UI: 9 a b c` (via the copy-backed cube), `4x RGBA8UI sample 2: 1 2 3 4`. `t_xlate` pins `texture3d<uint>` / `texturecube<int>` / `texture2d_ms<uint>` with Metal compiles. |
| 4. `make -C gfx test` | tested | ALL TESTS PASSED |
| 4. LAS corpus | tested | `NAPPLE_SHADER_CORPUS=/Users/neto/NappleWork/shader-corpus/las gfx/build/tests/t_xlate` -> `corpus las: 5767 programs, 5767 translated, 5767 compile with Metal` |
| 4. Four-title boots | **verified: no regression** | base 84cb5a6 vs head, 60 s (ACNH 90 s), A at 25 / 40 / 55 / 70 s, `MTL_DEBUG_LAYER=1`, cold private save copies, run lock. All 8 exit 0 with 0 skipped draws; warning sets identical (LAS 0, SMO 0, ACNH 0, MK8D 4 kinds); no Metal validation lines. Frames base -> head: ACNH 2675 -> 2679, LAS 3542 -> 3545, SMO 2400 -> 2387, MK8D 3508 -> 3554. Screenshot differences are timing (LAS f-1000 16, the animated intro; SMO f-2000 7.7); the others are <= 1.7. Logs `/Users/neto/NappleWork/logs/gfxgaps-reg-{base,head}-<titleid>/` |

## verified.json keys to flip (acceptance 5)
I don't own `.claude/skills/percent-analysis/verified.json`, so these are proposals:
- `nvn/nvnCommandBufferBindSeparateTexture`, `nvn/nvnCommandBufferBindSeparateSampler`, `nvn/nvnCommandBufferBindSeparateTextures`, `nvn/nvnCommandBufferBindSeparateSamplers`, `nvn/nvnCommandBufferBindSeparateTexturesDeferred`, `nvn/nvnCommandBufferBindSeparateSamplersDeferred`: `stub` -> `impl`. Note: replay + translator (pair of cbuf words), `t_sep_sampler`; the c2 table placement is a hypothesis; no title binds them.
- `gpu_fixed/depth/stencil formats`: `partial` -> `impl`. Note: D24 / D24S8 readback (`t_d24_readback`); stencil sampling still 2D / 2D array only (optional per the orchestration; a cube stencil view keeps the float path).
- `gpu_fixed/texture views / sparse`: `partial` -> `impl`. Note: integer 3D / cube / MS views declared (`t_int_tex` 4-6); cube-of-2D faces 1-5 replicate face 0 (a choice); no title uses sparse or integer views.

## Unbuilt / untested list
- Nothing unbuilt.
- No path here ran in a title (no title reads back D24, binds separate handles, or samples integer 3D / cube / MS views in the scenes reached).
- The separate c2 table offsets are unconfirmed.

## Findings
- No title of the four calls `BindSeparate*`: only lookups of the `Deferred` names, from before nvn-api implemented them, appear in old logs.
- Separate textures don't get depth / stencil / integer slot masks (`ds_slot_masks` / `int_slot_masks` look at combined slots), so a separate depth or integer texture keeps the float declaration. Left as is: no title uses one. A follow-up could resolve the pair's texture in the mask functions.
- `pair_detect` only marks unpredicated OR-like instructions whose sources are single constant buffer words; a handle built across branches or with shifts stays refused by name.

## How to integrate
- No ordering constraint. Conflict hotspot: `gfx/src/shader/maxwell.h` / `maxwell_translate.c` if nvn-object-layouts touched the translator (unlikely). `TRANSLATOR_VERSION` 17: take the higher number on conflict.
- Build: `make -C gfx test` -> ALL TESTS PASSED; `make -C runtime`.

## How to test
1. `make -C gfx test`: `t_d24_readback: ok`, `t_sep_sampler: ok`, `t_int_tex: ok (0 skipped)`, `t_xlate: ok`.
2. `NAPPLE_SHADER_CORPUS=/Users/neto/NappleWork/shader-corpus/las gfx/build/tests/t_xlate` -> 5767 / 5767 / 5767 (the other titles' corpora are there too; about 25 min for SMO).
3. Four-title boots as above; with a title that binds separate handles, look for `separate texture / sampler handles in use (first: c2[...] | c2[...] ...)`. If the offsets differ from 0x120 / 0x520, change `SEP_TEX_C2_BASE` / `SEP_SAMP_C2_BASE` in `gfx/src/shader/maxwell.h`.

## Fix policy hints
- Safe: `TRANSLATOR_VERSION` collisions; the `SEP_*_C2_*` constants once a real title shows its offsets.
- Needs a decision: separate textures in the depth / integer slot masks (see Findings).

## Needs from other areas
- None.
