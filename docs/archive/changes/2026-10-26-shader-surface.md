# 2026-10-26 shader-surface: SULD, SUST, SUATOM and SURED in the Maxwell translator

Branch `claude/shader-surface-2026-10-26-cloud2` (Linux cloud session; nothing here was compiled by Metal). Owner area: `gfx/src/shader/` and the shader text tests.

## What changed
- The surface family no longer ends in the "no image binding in the translator contract" refusal. `src/shader/surface.inc` (included by `maxwell_translate.c`) translates:
  - **SULD**: dimensions 1D, BUFFER, 1D_ARRAY, 2D, 2D_ARRAY, 3D; raw `.D` sizes U8 S8 U16 S16 32 64 128; formatted `.P` with a channel mask; byte addressing `.BA`; clamp modes IGN, TRAP, CLAMP, ZERO; immediate and bindless (`.B`) handles.
  - **SUST**: the same dimensions, sizes, masks and clamps; a partial `.P` mask is a read-modify-write.
  - **SUATOM / SURED**: ADD, MIN, MAX, AND, OR, XOR on 32-bit signed/unsigned images, through MSL 3.1 texture atomics.
- **Unsupported by Metal, each a translator error naming the instruction and the reason**: INC, DEC (wrapping), EXCH, CAS (MSL 3.1 has no texture exchange / compare-exchange), 64-bit and floating-point atomics. Also refused: more than 8 images, one image used with two dimensions, a key that contradicts a shader-forced integer kind, other opcodes in 0xea/0xeb.
- **Binding contract** (documented in `docs/architecture/gfx.md` section 11): image slot i is `[[texture(64 + i)]]`, access qualifier from use, component type from `ShaderKey.image_int_mask / image_uint_mask` or forced by raw/atomic access, handle at c2 + 4 * index or from a user buffer when bindless, `needs_msl31` for atomics. New `ShaderInfo` fields: `nimg`, `img[]`, `needs_msl31`; new `ShaderKey` fields: `image_int_mask`, `image_uint_mask`.

## Decisions
- The instruction field layout is the translator's own decode (listed at the top of `surface.inc`), reconstructed from memory of the public Maxwell decoders. It is pinned by hand-assembled tests but NOT checked against a shader binary from a game: the first Mac corpus run (`NAPPLE_SHADER_CORPUS`) is the real check. If a field is off, only `emit_surface` decode lines change.
- TRAP behaves as IGN (a GPU cannot trap). Bounds checks are explicit; Metal's own out-of-range behaviour is not relied on.
- `.BA` assumes the texel size equals the access size. A raw `.D` access without `.BA` returns the first channel(s) of a texel, not packed texel bits.
- Atomics promote an image to an integer kind on first use; an image already used as float is refused rather than silently retyped.
- Stretch: 1D-array surfaces are covered. TLD4/TLD4S edge cases were reviewed: the existing translation already handles array, depth, constant and AOFFI offsets; per-texel offsets (PTP) stay refused (Metal needs constant gather offsets) and were not changed.

## Status
| Item | Status |
|---|---|
| Translation text of every SULD/SUST/SUATOM/SURED variant above | tested (`sh gfx/tests/t_xlate_linux.sh`: ok) |
| MSL compiles (Metal) | unbuilt: `MSL_COMPILES` / `MSL_COMPILES31` in `t_xlate.m` run on the Mac only |
| Binding glue in `mtl_shader.m` / `mtl_replay.m` | not done (not mine); exact calls listed in gfx.md section 11 and the integrator file |
| Encoding against real shaders | proposed |

## Files
`gfx/src/shader/surface.inc` (new), `maxwell.h`, `maxwell_translate.c` (include, dispatch, argument list), `gfx/tests/t_surface.inc`, `t_surface_asm.inc` (new), `t_xlate.m`, `docs/architecture/gfx.md` (section 11 appended), `docs/guides/add-maxwell-shader-op.md` (one sentence).
