# Graphics design (libnapple_gfx)

## Decision 1: shader translation = native in-process Maxwell -> MSL translator
Evaluated on real shaders extracted from both target games (BNSH inside Link's Awakening .bfsha and Super Mario Odyssey .szs; extraction tools live in
git-ignored work/):

| Option | Result |
|---|---|
| (a) Ryujinx translator + SPIRV-Cross via NativeAOT service | Not usable here: no `dotnet` on the build machine (cannot rebuild or patch), the prebuilt binary is tuned to one game's driver-constant layout, needs a helper process (impossible on iOS), extra SPIR-V hop. Kept only as an offline reference. |
| (b) native C translator from LASRecomp (MIT) | 671/679 Link's Awakening shaders and 128/128 sampled SMO shaders translate; 295/295 sampled results compile with `metal -std=metal3.2`. In-process, no process spawn, ~1 ms per shader. Chosen. |
| (c) new translator | Not needed now. |

Cleanup of the imported translator (Vulkan GLSL path, per-game tint, debug hooks, mirror mode) is tracked in the change doc; regression check = MSL output of all
extracted shaders compared against a stored baseline. Known gaps: bindless TEX.B, a few unknown opcodes (8 LAS shaders), geometry/tessellation stages are skipped.

Control section facts used: +0x700 code length (constants start there), +0x6FC constants size, +0x714 stage, compute local size at +0x748/+0x74C/+0x750.
Texture handles are read from c2: combined handles at 0x20 + 8*slot.

## Decision 2: object model
Textures are their hardware descriptor (storage address, format, size, layout): NVNtexture objects, texture-pool entries and recorded render targets carry the
same snapshot and the Metal layer maps equal snapshots to one MTLTexture. Programs are shader-blob content hashes. Command lists are immutable, reference-counted
records published at EndRecording; uniform buffers are snapshotted per draw at replay (UpdateUniformBuffer patches memory in replay order).
Texture upload: block-linear -> linear on the CPU, dirty pages found by write protection (src/dirty.c; write-protect first, then read, so no write is lost).

## Decision 3: frame pacing
Guest vsync is virtual (1/60 s or 1/target_fps); presents are held with presentAfterMinimumDuration so a 30 fps game on a 120 Hz display is not sped up.
