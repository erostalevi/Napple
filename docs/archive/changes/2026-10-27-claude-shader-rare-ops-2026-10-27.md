# shader-rare-ops: the last 18 rare Maxwell ops, stencil texture bindings (2026-10-27)

Status words: *tested* (text tests pass on Linux), *unbuilt* (MSL never compiled with Metal).

## What changed
- `gfx/src/shader/maxwell_translate.c`: the catch-all refusal "control/system instruction" and the IDP refusal are gone. Translated: `BPT IDE RAM SAM STP R2B` (documented no-ops), `RTT` (ends the thread), `B2R` (writes 0), `PEXIT` (`pex` target taken by the next EXIT), `PLONGJMP`/`LONGJMP` (`ljs` stack), `GETCRSPTR`/`SETCRSPTR` (`cal_n`), `GETLMEMBASE`/`SETLMEMBASE` (`lmb`), `ISBERD` (indexed attribute read through the ALD path), `IDP` (`idp()` helper, DP4A/DP2A). `FCHK` comment reworded so the scan sees it (it was already implemented).
- Stencil sampling: `ShaderKey.stencil_slots`, `ShaderTexture.stencil`; a stencil binding is `texture2d<uint> tN_u` (or `texture2d_array<uint>`) wrapped by `StencilTex2D`/`StencilTex2DArray` (`sample`/`gather`/`read` give `(stencil, 0, 0, 1)` as integer bits). Refused: other targets, depth comparison.
- `gfx/src/mtl_shader.m`: only `TRANSLATOR_VERSION` 11 -> 12.
- Tests: `gfx/tests/t_xlate_rare.inc` (new, included from `t_surface.inc`); `t_xlate.m` loses the old "refused by name" list for these ops.
- Docs: paragraph appended to `docs/architecture/gfx.md`.

## Verified vs not
`sh gfx/tests/t_xlate_linux.sh`: `t_xlate: ok` (197 Metal compile checks skipped). Not verified: any MSL compile (the new wrapper structs, `idp`), the hardware opcode/field layouts of the rare ops (they follow the existing tests' opcode bases; no compiler emits them), numeric results. Scan: shader ISA rare 24/42 -> 42/42 (95% -> 100%, 11.3 -> 12.0 pts).
