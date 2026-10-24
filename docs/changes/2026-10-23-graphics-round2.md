# Graphics round 2: LAS in-game white-out root cause (partial round, stopped at a clean point)

## Objective
Fix general graphics correctness shared by Link's Awakening (LAS) and Super Mario Odyssey (SMO); first target: LAS in-game scenes rendering flat white.

## Root cause (LAS white room) - FIXED
`nvnVertexAttribState` format `0x69` (NVN_FORMAT_RGB10A2SN, packed signed normal/tangent) had no Metal vertex format in `mtl_vertex_format`. The attribute was dropped,
the vertex shader read zeros for the tangent frame, the PBR fragment shaders normalised a zero vector (NaN), and the final `fmin(x, 4.0)` (Metal fmin returns the
non-NaN operand) turned every lit pixel into the HDR clamp value 4.0, which tone mapping and bloom showed as white. Fix: `0x69 -> MTLVertexFormatInt1010102Normalized`,
plus the whole `*_UI2F/*_I2F` family (`0x51..0x68`, plain integer formats feeding float attributes) and BGRA8 (`0x9A`).
Evidence: before `work/shots/user-first-room.png`, after `work/shots/las-room-after-fix.png` (same scene, Marin's house, scripted input).

## Second bug found on the way (compute) - FIXED
Compute shaders read uniform buffers through global memory; `bind_stage` stored host addresses in the driver-constant UBO table (c0+0x210) instead of Metal GPU
addresses (SSBOs already used `gpu_va`). The light-culling compute (30x17 tiles) therefore saw no lights. Now all three lights per tile are listed.

## Files
- `gfx/src/mtl_tex.m` (vertex formats), `gfx/src/mtl_replay.m` (UBO gpu_va; diagnostics), `gfx/src/mtl_shader.m`, `gfx/src/shader/maxwell_translate.c|maxwell.h` (probe), `gfx/src/mtl_core.m` (event screenshots),
  `gfx/src/shader/blob.c` (compute control dump at log level 3), `gfx/tests/t_vfmt.m` (new regression test), `gfx/tests/t_xlate.m` (corpus lists failing shaders).

## Diagnostics added (all env switches, no per-game logic)
- `NAPPLE_GFX_EVENT_FS=<fs hash>` starts a relative frame clock at the first draw with that fragment program (machine-load independent captures);
  `NAPPLE_GFX_EVENT_SHOTS=n,n,..` + `NAPPLE_GFX_EVENT_PREFIX` take screenshots at relative frames; `NAPPLE_DRAWLOG` frames become relative.
- `NAPPLE_DRAWLOG_QUIET`, `_GRID` (sample grid of pixel changes per draw, with `_EACH`), `_UBO=<words>`, per-pass RT dumps from every pass end, vertex attribute/stream and compute (UBO/SSBO after) logging.
- `NAPPLE_SHADER_PROBE=<fs>:<pc hex>:<rA>:<rB>:<rC>[:scale][;...]` (+`_PERIOD`): a fragment program writes intermediate registers as its colour (debugging; compiled synchronously). Use `NAPPLE_SAVE_MSL=1` with an empty shader cache to get `// pc` annotations.
- Scripted LAS path to the first room: `NAPPLE_MOVIE_PACE=0 NAPPLE_TURBO=1000`, L+R at ~60 s then A every 4 s (reaches Marin's room in ~6 minutes under load).

## Findings not yet acted on
- Translator: 6 LAS shaders fail (3x `bezel_pbr*` ps: unknown opcode `0x7e06...` probably HSETP2 immediate/cbuf form; `decal_uber` ps: AL2P `0xefa0` + indexed attribute; `water_uber` ps: bindless TEX.B handle not from a cbuf). SMO corpus 128/128.
- `mtl_pipe_get` skips draws while an async pipeline compiles (visible as pops and wasted frames); a fallback or prewarm is needed.
- `OP_DRAW_TEXTURE`, transform feedback, multi-draw-indirect-count, separate texture/sampler binding remain unimplemented; `nvnMemoryPoolMapVirtual` is implemented (t_vpool).
- SMO text: a run by the other engineer (work/smo/j3-3500.png) already shows glyphs ("Look Around"); not re-verified by me (lock taken before my SMO run).
- Render scale (rscale plumbing), settings, 60 fps work, shader disk-cache prewarm: not started.

## Tests
`make -C gfx test` passes (new `t_vfmt`), Metal validation layer on. Game runs: LAS to Marin's room headless; intro movie and title confirmed earlier in the session (work/g2/a1-*, a3-*).

## Risks
Debug switches add a few getenv calls on hot paths (cached statics); remove if profiling shows cost.
