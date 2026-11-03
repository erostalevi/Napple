# shader-stages (2026-10-27): translator side of geometry, tessellation, transform feedback; OUT, VABSDIFF4, TXA, IMADSP

Branch `claude/shader-stages-2026-10-27` (Linux cloud: translator text *tested*, nothing compiled against Metal). Contract: [gfx.md section 13](../../architecture/gfx.md); tests: `gfx/tests/t_xlate_stages.inc`.

## What changed
- **Stages beyond VS/FS/CS** in `gfx/src/shader/maxwell_translate.c`: `STAGE_GEOMETRY` (compute kernel, one thread per input primitive, output vertex records + index buffer with primitive restart), `STAGE_TESS_CTRL` (compute kernel, one threadgroup per patch, writes control-point records, a patch record and Metal tessellation factors), `STAGE_TESS_EVAL` (post-tessellation vertex function). Driven by new `ShaderKey` fields; buffer indices, record layouts and dispatch shapes are macros in `maxwell.h`.
- **Transform feedback / stage-out** for the vertex stage: `ShaderKey.xfb[]` and `stage_out` add `xfb_ctl`, `xfb<k>` and `stage_out` buffers and a capture epilogue ahead of the clip-space flips. Geometry output is captured by the replay from the same record layout; tess eval and non-vertex stages are refused by name.
- **OUT** (reg / imm / cbuf forms, EMIT / CUT / EMIT_THEN_CUT), **VABSDIFF4** (add / min / max / packed, signed lanes, SAT), **IMADSP** (six sub-word selectors, 0x5a / 0x4a / 0x34 / 0x52 classes), **TXA** (read like TXQ). Their `fail()` strings are gone; OUT outside a geometry shader still fails with "OUT". Unsupported sub-forms are refused by name (lane selectors, `.CC` / `.HI`, reserved selectors, other TXA selectors).
- `ALD` / `AST` decode the vertex register (bits 39..46), patch bit (31) and output-read bit (32); the vertex index is latched before the destination registers are written. `S2R SR_INVOCATION_ID` (0x11) is the tess control output control point.
- `TRANSLATOR_VERSION` 10 to 11 (`gfx/src/mtl_shader.m`, one line outside `gfx/src/shader/`, as acceptance asks).
- `gfx/tests/t_xlate.m`: removed the three refusal cases for VABSDIFF4, TXA and IMADSP (they assert the old behaviour); `t_surface.inc` ends with `#include "t_xlate_stages.inc"` so no other line of `t_xlate.m` changes.

## Decisions (made without asking)
- Bit layouts of VABSDIFF4 and IMADSP, and TXA's reading as TXQ, are the translator's own (no public decoder in the repo, no shader that uses them); the tests pin the reading, not the hardware. Selector forms we cannot define are refused, never guessed.
- Geometry: the replay assembles input primitives into a list; the kernel writes fixed regions (`gs_max_vertices` records, `2 * gs_max_vertices` indices per primitive) so no atomics and a deterministic order; GS instancing, multiple streams and isolines are out of scope and refused or documented.
- Tess levels are written to Metal's factor buffer only when the program stores them; otherwise the replay supplies the NVN defaults.

## Verified vs not
| Item | Status |
|---|---|
| Text of every new path (`sh gfx/tests/t_xlate_linux.sh`: ok, 0 FAIL) | tested |
| Generated GS / TCS / TES / VS-capture / op MSL parses as C++ against a throwaway shim (kept out of the tree) | tested (syntax only) |
| MSL compiles in Metal (`MSL_COMPILES` for all new cases in `t_xlate.m`) | unbuilt |
| Replay side (VS capture pass, list assembly, GS dispatch and draw, `drawPatches`, XFB counters) | not written |
