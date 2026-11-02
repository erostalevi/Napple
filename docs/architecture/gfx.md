# Graphics: libnapple_gfx design and interface

This page has two parts: the design decisions, then the interface between the runtime and the gfx library.

## Design decisions
### Decision 1: shader translation = native in-process Maxwell -> MSL translator
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

### Decision 2: object model
Textures are their hardware descriptor (storage address, format, size, layout): NVNtexture objects, texture-pool entries and recorded render targets carry the
same snapshot and the Metal layer maps equal snapshots to one MTLTexture. Programs are shader-blob content hashes. Command lists are immutable, reference-counted
records published at EndRecording; uniform buffers are snapshotted per draw at replay (UpdateUniformBuffer patches memory in replay order).
Texture upload: block-linear -> linear on the CPU, dirty pages found by write protection (src/dirty.c; write-protect first, then read, so no write is lost).

### Decision 3: frame pacing
Guest vsync is virtual (1/60 s or 1/target_fps); presents are held with presentAfterMinimumDuration so a 30 fps game on a 120 Hz display is not sped up.

## Interface (what the runtime provides and calls)

Header: `gfx/include/napple_gfx.h` (pure C). Library: `gfx/libnapple_gfx.a` (`make -C gfx`). Design: the decisions above.
The authoritative definition is the header; this page explains it. History of changes: the dated graphics notes in [the change archive](../archive/changes/README.md).

### 1. Lifecycle

```
runtime start
  napple_gfx_config_default(&cfg)      fill: metal_layer (or NULL headless), title_id, cache_dir, hooks, settings
  napple_gfx_init(&cfg)                creates MTLDevice/queue + gfx threads; 0 = ok
  napple_gfx_hle_table(&n)             register each {symbol, fn} as a host override of the guest import of that name
guest runs: nnMain -> nn::gfx -> nv::SetGraphicsAllocator / nv::InitializeGraphics / nvnBootstrapLoader("nvnDeviceGetProcAddress") ...
every guest present -> hooks.on_present(frame, swap_interval) -> runtime signals vsync event at napple_gfx_vsync_period_ns()
exit: napple_gfx_diag_dump(); napple_gfx_shutdown()
```

### 2. Symbols the runtime must redirect

`napple_gfx_hle_table()` returns the authoritative list. At the time of writing:

| guest symbol | meaning |
|---|---|
| `nvnBootstrapLoader` | returns the NVN entry points; the SDK's own NVN driver is never executed |
| (not overridden) `nv::InitializeGraphics`, `nv::SetGraphicsAllocator`, `nv::SetGraphicsDevtoolsAllocator` | run natively; overriding the allocators crashes SMO, and InitializeGraphics needs the runtime's nvdrv (see docs/architecture/runtime-services.md) |

Every function pointer gfx returns to the guest goes through `hooks.wrap_guest_callable(user, fn, name)`
(use it to run NVN calls on the guest thread's host stack, as the reference does with `runtime_hle_wrap_named`).
The guest calls NVN entry points with the plain AAPCS64 ABI; there are no stack arguments, up to 8 integer and 8 FP
register arguments. Boolean (`NVNboolean`) parameters are not extended by Switch callers: gfx masks to 8 bits.

### 3. Guest memory: what gfx needs from the runtime

1. **Identity mapping.** A GPU address passed by the game is a host virtual address. NVN memory pools wrap game memory
   (`nvnMemoryPoolInitialize(storage, size)`); gfx wraps the same pages in a zero-copy `MTLBuffer`
   (`newBufferWithBytesNoCopy`), so the pages must be page-aligned `mmap` memory owned by the process (16 KiB pages on
   Apple Silicon; the guest sees 4 KiB granularity, gfx rounds outward to 16 KiB).
2. **Virtual memory pools** (`NVN_MEMORY_POOL_FLAGS_VIRTUAL`, `nvnMemoryPoolMapVirtual`): gfx remaps physical pool pages
   (`mach_vm_remap`) so the virtual address range must be reserved by the guest/runtime as normal anonymous memory.
3. **Write-fault notifications (dirty tracking).** The reference marks pool pages that hold textures read-only after
   uploading them to the GPU; a guest write faults, the page is marked dirty and unprotected. Therefore:
   - the runtime's fault handler (Mach exception server or SIGSEGV/SIGBUS handler) must call
     `napple_gfx_handle_fault(addr, is_write, ucontext)` first for every access fault inside guest memory; on `1` it
     retries the instruction;
   - the runtime must call `napple_gfx_guest_write_prepare(addr, len)` before any *host-side* write into guest memory
     that a syscall performs on behalf of the guest (file reads, IPC out-buffers, decoders): a kernel `read()` into a
     protected page returns `EFAULT` instead of faulting through the handler;
   - the runtime must not itself `mprotect` ranges that were handed to nvn memory pools.
4. **No JIT / no x18 assumptions in gfx.** gfx code is plain host code. It never touches guest registers.

### 4. Threading

- The guest's graphics thread(s) call NVN entry points directly. gfx implements them on that thread, records, and may
  hand work to a private translation/replay thread (`submit` returns before the GPU work is built).
- Blocking entries use `hooks.core_away`/`core_back`.
- gfx never calls back into guest code, except through addresses the game passed itself (debug callbacks, allocators):
  `nv::SetGraphicsAllocator` callbacks are stored and may be invoked from the guest thread that is inside the NVN
  call. The runtime does not need to do anything for these.

### 5. Presentation, resolution and frame pacing

- `metal_layer`: a `CAMetalLayer*`. gfx sets pixel format, `framebufferOnly`, `displaySyncEnabled`, drawable size. NULL
  selects headless mode (offscreen texture, same code path, used by tests and `napple run --headless`).
- `settings.output_width/height`: custom output size; `internal_res_scale`: render-target scale for scene-sized targets
  (window-derived textures are scaled, fixed-size targets such as shadow maps are not); `upscaler`: none/bilinear/MetalFX.
- Pacing: the runtime owns the guest-visible vsync event. It must fire it every `napple_gfx_vsync_period_ns()`, **not**
  every game swap interval. gfx decides when to actually show frames: `NAPPLE_PACING_GAME` paces to
  `swap_interval/60 s`, `NAPPLE_PACING_FIXED` to `target_fps`. Speed-up artifacts happen when game logic is stepped by
  vsync counts: for a game that requests 30 Hz (swap interval 2), `game_fps_divisor_fix=1` keeps the guest's logical
  step at 2 vsyncs while gfx may present interpolated/duplicated frames at the higher output rate. (Games that step logic
  from the Horizon tick instead of vsync need no fix; see the "Decision 3: frame pacing" section above.)
- `napple_gfx_screenshot(path)`: PNG of the next presented frame, for headless verification.

### 6. Diagnostics

`napple_gfx_diag_unimplemented()` returns distinct unimplemented NVN entry points that the guest called, with counts;
each is logged once by name at WARN as it happens. `settings.unimplemented_abort=1` aborts on the first one (CI).
`napple_gfx_diag_unknown_lookups()` lists names asked of `nvnDeviceGetProcAddress` that this build does not know.
`napple_gfx_get_stats()` gives frames, draws, pipelines compiled, shader cache hits, pending async compiles.

### 7. Integration status

Pending: runtime/ has no loader yet (empty directory when this was written). When it does, the glue is: init, apply
`napple_gfx_hle_table()` to the module import resolver, forward fault handler and write-prepare, drive vsync from
`on_present` and `napple_gfx_vsync_period_ns()`.

### 8. Updates since first publication

- `napple_gfx_config.shader_translator` is reserved (NULL): translation is in-process (see the design decisions above); no helper process, no .NET.
- `napple_gfx_settings.present_interval_override` added (0 = honour the game). The guest vsync event is virtual: fire it every
  `napple_gfx_vsync_period_ns()` (1/60 s by default), not at the monitor refresh.
- NVN API version: the game checks `nvnDeviceGetInteger(API_MAJOR/MINOR)` against the version it was built with, so the runtime must put the
  game's version in `nvn_major/nvn_minor` (per-game data; 0,0 = 55.15, right for Link's Awakening).
- Shader global memory (LDG/STG, used by SSBO access) is addressed through one buffer over the guest heap: the runtime sets `gmem_base`/`gmem_size` in the config (page aligned; 0 = none, and SSBO-heavy compute shaders are then skipped).
- Status: the API layer, the Metal replay layer (`gfx/src/mtl_replay.m`) and presentation are implemented; the four reference titles render (per-title state in [titles/](../titles/README.md)).

### 9. Integration (done on the graphics side, verified up to the guest's first nvdrv call)

- `make -C gfx lib adapter` builds `gfx/libnapple_gfx.a` and `gfx/build/obj/adapter/gfx_napple.o`, which implements runtime/src/gfx.h on top of
  napple_gfx.h (hooks mapped: `core_away` token, `wrap_guest_callable`, `on_present`).
- `make -C runtime GFX=napple` links them instead of `gfx_stub.c` (the only change made under runtime/: a conditional block in its Makefile).
- The runtime forwards guest faults to `gfx_handle_fault` from `runtime/src/signals.c` (guest write tracking, `gfx/src/dirty.c`).
- Per game the runtime must set `nvn_major/nvn_minor` (from `game.json`) and the heap window (`gmem_base/gmem_size`) if SSBO compute is used.

### 10. Integration findings (2026-10-22)
- The adapter builds its override table lazily: the runtime registers overrides before `gfx_init`.
- Window origin: "lower-left" (the NVN default) changes only window coordinates (viewport, scissor, fragment coordinate y = H - y_top via the rscale table); NDC +y renders to the top row in both modes. The former vertex-shader Y negation produced upside-down images in real games and was removed.
- `CopyTextureToTexture` with a different size or format is done by a draw (format conversion and scaling); depth/MSAA/3D variants are still not implemented.
- A recorded command list stays valid as long as the command memory it was recorded into: it ends when the game finalizes the handle, when another list is recorded at the same command-memory address, or when the pool holding that memory is finalized (never with its command buffer, by idle time or by a memory budget; Super Mario Odyssey replays lists for minutes). Idle lists are held LZ4-packed. See the [2026-10-26 gfx-correctness note](../archive/changes/2026-10-26-claude-gfx-correctness-2026-10-26-macos1.md).
- Headless runs pace presents at the swap interval so the guest runs at real speed.
- New host API: `napple_gfx_texture_info`, `napple_gfx_texture_write_rgba` (video decoders).
- Per-game: `nvn_major/nvn_minor` from `game.json`.

### 11. Surface images (SULD, SUST, SUATOM, SURED)

The translator turns the Maxwell surface instructions into Metal texture `read` / `write` / `atomic_fetch_*` calls (`gfx/src/shader/surface.inc`). Status: *tested* on Linux (`sh gfx/tests/t_xlate_linux.sh`: text assertions on hand-assembled words); *unbuilt* against Metal and never run on a real shader binary. The instruction field positions follow the public Maxwell decoders from memory; the corpus run on the Mac (`NAPPLE_SHADER_CORPUS`) is the first check against real code.

**Binding contract (translator output -> Metal runtime).** `ShaderInfo.nimg` / `img[i]` (`shader/maxwell.h`, `ShaderImage`) list the images in order of first use, at most `MAX_IMAGES` (8).

| Item | Rule |
|---|---|
| Metal texture index | image slot `i` is `[[texture(IMAGE_TEXTURE_BASE + i)]]`, base 64. Sampled textures use 0..62, so the two never collide. No sampler. Declared after the sampled textures, in all stages (compute, fragment, vertex). |
| MSL argument | `texture<dim><T, access::A> im<i>`; `dim` from `ShaderImage.type`: 1D `texture1d`, 1D array `texture1d_array`, 2D `texture2d`, 2D array `texture2d_array`, 3D `texture3d`, buffer `texture_buffer`. |
| Component type `T` | `float` by default. `int` / `uint` when the pipeline key says so (`ShaderKey.image_int_mask` / `image_uint_mask`, bit n = SULD/SUST immediate index n, bit 31 = every bindless image), or when the shader forces it (raw `.U8/.S8/.U16/.S16` access, atomics): then `ShaderImage.kind` is `IMGKIND_INT` / `IMGKIND_UINT` and the runtime must supply a matching format (R8Uint ... RGBA32Uint, R32Uint / R32Sint for atomics). A key that contradicts the shader is a translator error naming the instruction. |
| Access `A` | `read`, `write` or `read_write` from `ShaderImage.access` (`IMGACC_READ`, `IMGACC_WRITE`): the union of the instructions that use the image. The runtime creates the texture with the matching usage (`MTLTextureUsageShaderRead`, `ShaderWrite`; atomics also `MTLTextureUsageShaderAtomic`), and `read_write` needs `MTLReadWriteTextureTier` 1 (R32 formats) or 2 (all). |
| Handle | `ShaderImage.handle_offset`: byte offset in c2 of the NVN image handle (immediate index * 4), exactly like `ShaderTexture.handle_offset`. A bindless handle (`.B` forms, `ShaderImage.bindless`) is read from user uniform buffer `bindless_cbuf` at `bindless_offset`; a bindless handle that comes from c2 is an ordinary table entry. |
| Language version | `ShaderInfo.needs_msl31`: the MSL contains texture atomics and must be compiled with `MTLLanguageVersion3_1` (macOS 14+). Other shaders compile as before. |
| Out of range | per instruction clamp mode: IGN and TRAP skip the access (a load returns 0, a store is dropped; there is no trap on a GPU), ZERO the same, CLAMP clamps each coordinate and the layer to the extent. The checks are explicit, not left to Metal. |

**What is translated.**

- SULD / SUST, dimensions 1D, BUFFER, 1D_ARRAY, 2D, 2D_ARRAY, 3D (coordinates in consecutive registers from Ra: x, y, z, and the array layer last). `.D` raw sizes U8 S8 U16 S16 32 64 128 (1, 1, 1, 1, 1, 2, 4 registers; sub-word sizes need an integer image), `.P` formatted access with a channel mask (channels compact into consecutive registers; a partial-mask store is a read-modify-write and makes the image `read_write`), `.BA` byte addressing (x is divided by the access size, which assumes the texel size equals the access size).
- SUATOM (returns the old value) and SURED (no result, data in Rd): ADD, MIN, MAX, AND, OR, XOR on 32-bit signed / unsigned integer images. MIN / MAX use the signedness of the instruction type, and the image is declared `int` or `uint` accordingly.
- Unsupported, each a translator error naming the instruction (for example `SUATOM.EXCH.2D: ...`): INC and DEC (wrapping compare-and-update: no Metal texture atomic), EXCH and CAS (MSL 3.1 texture atomics are fetch_add/sub/and/or/xor/min/max only), 64-bit and floating-point atomics, more than 8 distinct images, one image used with two dimensions, other opcodes of the 0xea / 0xeb range.

**Known approximations.** A raw `.D` access without `.BA` returns the first channel(s) of the texel, not the packed texel bits; formatted UNORM/SNORM data comes back as Metal's converted float, the same value `.P` returns on hardware. Float-image `.D.32` moves the bit pattern through the float channel (NaN payloads may be canonicalised by the texture unit).

**The Mac glue** (`gfx/src/mtl_replay.m` `bind_stage` image loop and `image_masks`, `mtl_tex.m` `mtl_tex_image_view`, `mtl_shader.m` key masks and `compile_options_v`; 2026-10-26 gfx-correctness, test `gfx/tests/t_image.c`: a fragment SUST.D.32 into an R32UI image read back exactly) does the following:

1. After `maxwell_translate`, bind each `info->img[i]` texture with `setTexture:atIndex:IMAGE_TEXTURE_BASE + i` on the compute / fragment / vertex encoder: resolve the handle from c2 at `handle_offset` (or the bound user buffer at `bindless_cbuf` / `bindless_offset`) through the same TIC lookup as sampled textures.
2. Make the texture a view with `MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite` (plus `ShaderAtomic` when `img[i].atomic`) and an integer pixel format when `kind` is `INT` / `UINT`; sRGB views cannot be written, use the linear twin.
3. Fill `ShaderKey.image_int_mask` / `image_uint_mask` from the formats of the bound images when building the pipeline key (pipelines for the same shader with different formats are different pipelines).
4. Compile with `MTLLanguageVersion3_1` when `needs_msl31`.
5. `useResource:usage:` is not needed unless the images are reached through an argument buffer.
6. A shader-written image makes the Metal texture the owner of the data (`GfxTex.gpu_owned`, as for render targets): CPU uploads stop, reads go to the GPU copy; there is no write-back into guest pages (the same as render targets today).

### 12. Render scale, output fit and upscaling (2026-10-27)

The player-facing "Video" settings (overlay tab, `settings.json`, `napple_display_settings` in `gfx/include/napple_display.h`) are applied live: a change reaches the
next frame, and the game's screen-sized render targets are rebuilt after the window size was stable for 300 ms.

**Vocabulary.** *base size* = what the game believes its screen is (1920x1080 docked, 1280x720 handheld, the window's aspect when "match window aspect" is on);
*render size* = base x scale, the real size of the screen-sized targets; *output size* = the drawable.

**Render scale.** `render_mode` 0 native, 1 scale factor, 2 follow the window, 3 fixed size. The factor is free between 10 % and 800 % (`internal_scale_percent`;
presets 0.5x / 1x / 2x / 4x and "4K" / "1440p" targets in the overlay, a text field for any value). `supersample_percent` (100..400) multiplies it and is filtered
down again by the output resamplers (box / bicubic / Lanczos kernels stretched by the down-scale ratio). `mtl_scale.m` allocates the screen-sized targets at
`w * sx, h * sy`; viewports, scissors, texel fetches, size queries and the fragment position go through the `rscale` table the shader translator reads.
Which textures count as screen sized: `mtl_scale_pick_size` (same shape as the swapchain, not smaller than base / 8, not larger than the screen, plain 2D, not mipped, not
video decode / linear / sparse, rendered to by the GPU or depth). `NVN_TEXTURE_FLAGS_IMAGE` is **not** a veto for GPU-written targets (LAS, SMO and ACNH set it on every
screen-sized target; the old veto left all of them unscaled). `NAPPLE_SCALE_DIAG=1` logs every distinct target and the decision once (`scale-diag:` lines).

**Budget.** `napple_display_budget_pixels(working set)` (about 48 Mpixel, `recommendedMaxWorkingSetSize / 220` on small GPUs). `napple_display_compute_scale` gives up
supersampling first (down to 1x), then reduces the render scale uniformly, and never exceeds 16384 per side: 4x with 4x supersampling degrades to about 2.7x instead of
failing. `g_scaled_bytes` and the cache trim see the larger targets.

**Output fit** (`fit`, from the overlay "Window" setting): letterbox / pillarbox (bars), stretch, fill (crop the long axis, centred) and whole-number multiple with bars.
`napple_display_fit_rect` is a pure function (unit tested for 16:9, 21:9, 16:10, 4:3 and 9:16 targets). "Match window aspect" additionally tells the game a different
screen size through vi/applet (`napple_display_game_size`); a title whose HUD or camera breaks with it is marked `"report_aspect": false` in the per-title table
(HUD-safe fallback: it is told the native size, the picture is still fitted).

**Upscalers** (`upscaler`): 0 none, 1 MetalFX spatial, 2 built-in edge adaptive (`mtl_upscale.m`: structure tensor driven anisotropic Lanczos-2 with an anti-ringing clamp,
then a limited-lobe sharpening pass; original code, no third-party shader), 3 CAS (Catmull-Rom plus contrast adaptive sharpening). With an upscaler the render size is
window / `upscale_factor` (1.5) and the upscaler brings it up. `napple_display_effective_upscaler` answers what is really used: MetalFX absent (old OS, simulator SDK)
falls back to 2 and the overlay says so. MetalFX *temporal* needs depth and motion vectors from the game; none of the supported titles provides them, so it is never
offered (decision, 2026-10-27). `sharpness` 0..1 scales the sharpening of 2 and 3.

**Per-title table.** `mtl_scale_table.m`, format in its header: `{"titles": {"<title id>": {"disable", "max_scale", "min_div", "skip": [[w,h]], "force": [[w,h]],
"report_aspect"}}}`; built in defaults plus an optional file named by `NAPPLE_RSCALE_TABLE`. The mod / fix system will ship the same format. See
`docs/archive/changes/2026-10-27-claude-render-scale-2026-10-27.md` for what the audit of the four titles put in it.

**Tests.** `make -C gfx test`: `t_scale_math` (factors 0.5 / 1 / 1.5 / 2 / 4 / 1.37, allocation sizes, viewport and scissor conversion, rscale entries, budget, fit rectangles,
upscaler fallback), `t_scale_upscale` (upscaler matrix with PSNR and sharpness asserts, writes the contact sheet), `t_scale_table`, `t_display`. Real games:
`tools/golden/render_scale.py shots|stress` (headless drawable size from `NAPPLE_OUTPUT_SIZE=WxH`).

Evidence: [screenshots of the four titles at 1x / 2x / 4x, 21:9, 9:16 and with each upscaler](../screenshots/render-scale/README.md).

### 13. Geometry, tessellation and transform feedback (2026-10-27)

Metal has no geometry stage and no transform feedback, and its tessellation is split differently from Maxwell's. The translator (`gfx/src/shader/maxwell_translate.c`) therefore emits compute kernels and vertex functions that read and write plain buffers, and the Metal replay layer wires the passes together. Status: the MSL text is *tested* on Linux (`sh gfx/tests/t_xlate_linux.sh`, `gfx/tests/t_xlate_stages.inc`); every case compiles in Metal (`MSL_COMPILES` in `t_xlate.m`, *tested* on the Mac 2026-10-27), and the replay side is `gfx/src/mtl_stages.inc` (see **Replay** below). All buffer indices and record layouts are macros in `shader/maxwell.h`; the key fields that drive them are in `ShaderKey`.

**Stage record.** Everything that travels between stages is an array of records of `STAGE_SLOTS` (34) `float4`: slot 0 = system values (attribute 0x60..0x6F: x primitive id, y layer, z viewport index, w point size), slot 1 = position (0x70), slot `2 + n` = generic attribute n (0x80 + 16 n). `ShaderInfo.stage_slot_mask` says which slots a program stores; the others may be left unwritten. Patch records (tessellation) have the same size: slot 0 = outer levels (0x00..0x0C), slot 1 = inner levels (0x10, 0x14), slot `2 + n` = generic patch attribute n (0x20 + 16 n); `ShaderInfo.patch_slot_mask`.

**Vertex capture (VS).** `ShaderKey.stage_out` makes the vertex function also write its record to `device float4 *stage_out [[buffer(29)]]` at `xo * STAGE_SLOTS`; `ShaderKey.xfb[]` (`xfb_count` entries, `xfb_stride[k]` bytes per vertex) writes the named components to `device uint *xfb<k> [[buffer(24 + k)]]` (only the buffers in `ShaderInfo.xfb_buffer_mask` are declared). Both use `constant uint *xfb_ctl [[buffer(28)]]`: `[0]` vertices per instance, `[1]` first vertex, `[2 + k]` start of xfb buffer k in 32-bit words (the NVN buffer offset plus the counter, divided by 4). The vertex ordinal is `xo = xfb_ctl[0] * instance_id + (vertex_id - xfb_ctl[1])`, so the replay draws **non-indexed** (it expands an indexed draw into a vertex sequence first, as the hardware captures in processing order). The values are the shader's own outputs, taken before the `flip_y` / `depth_minus_one` conversions. An entry's `addr` is the Maxwell attribute address of its first component (0x70 position, 0x80 + 16 n generic, 0x60..0x6F system values); an output the program never wrote captures as 0. The replay draws these passes with `rasterizationEnabled = NO` (`MTLRenderPipelineDescriptor`). Fragment, compute, geometry and tess-eval keys carrying `xfb_count` / `stage_out` are refused with a named error (geometry output is captured from `gs_vtx` / `gs_idx` by a copy pass using the same entry table; tess-eval output only exists after tessellation).

**Geometry shaders (`STAGE_GEOMETRY`).** A compute kernel, **one thread per input primitive**, dispatched over `sp[0]` threads (the kernel returns when `gid >= sp[0]`). Key: `gs_max_vertices` (1..1024, required), `gs_in_vertices` (1, 2, 3, 4 lines+adjacency, 6 triangles+adjacency; default 3), `gs_out_topology` (`GS_OUT_*`).

| Buffer | Binding | Contents |
|---|---|---|
| `stage_in` | 19, `device const float4 *` | the vertex stage's `stage_out` records, **assembled as a list**: primitive p reads records `sp[1] + p * gs_in_vertices + v`. The replay expands strips / fans / indexed draws into this list |
| `sp` | 20, `constant uint *` | `[0]` input primitives, `[1]` first vertex record, `[2]` primitive id of thread 0 |
| `gs_vtx` | 21, `device float4 *` | output vertex records, `gs_max_vertices` per input primitive: vertex n of primitive p is record `p * gs_max_vertices + n`. The replay keys the kernel without `flip_y` / `depth_minus_one` (since b8): the pass-through vertex function applies them, so the records stay the program's own values for transform feedback |
| `gs_idx` | 22, `device uint *` | `2 * gs_max_vertices` indices per input primitive, pre-filled with `0xFFFFFFFF` (primitive restart) by the kernel. EMIT appends the vertex's record index, CUT appends one restart |
| constant buffers, textures, images, gmem, rscale | as every stage | |

The replay draws `gs_idx` (indexed, restart enabled, `gs_out_topology`) over `gs_vtx` with a pass-through vertex function that fetches the slots in `stage_slot_mask`; the fragment shader then reads the generic slots as ordinary varyings. `ALD a[v][addr]` reads input vertex `v` (the register in bits 39..46; none = vertex 0, indexed reads select the slot at run time); attribute 0x60 reads the primitive id; `AST` fills the current output vertex; `OUT` mode 1 = EMIT, 2 = CUT, 3 = both (stream operand ignored, stream 0 only); `S2R SR_INVOCATION_ID` is 0 (one invocation per primitive; geometry instancing is not translated). Output beyond `gs_max_vertices` is dropped. Per-primitive layer / viewport index stores land in slot 0 (`.y` / `.z`); the pass-through vertex function routes them (see viewport arrays below).

**Tessellation.** `ShaderKey.tess_out_vertices` (output control points = invocations per patch, 1..32), `tess_in_vertices` (control points of an input patch, tess control only), `tess_domain` (`TESS_DOMAIN_QUAD` / `TRIANGLE`; isolines are refused: Metal has no isoline patches).

*Tess control (`STAGE_TESS_CTRL`)*: compute kernel, **one threadgroup per patch** (`sp[0]` threadgroups), `tess_out_vertices` threads per threadgroup; `S2R SR_INVOCATION_ID` is the thread (output control point), `BAR` is a threadgroup barrier that also orders device memory. Inputs: `stage_in` (19, record `sp[1] + patch * tess_in_vertices + v`, from the vertex stage's `stage_out`) and `sp` (20, `[2]` first patch id). Outputs: `tess_vtx` (21, `device float4 *`, record `patch * tess_out_vertices + invocation`, written by plain `AST`), `tess_patch` (22, `device float4 *`, patch record `patch`, written by `AST.P`), `tess_fac` (23, `device half *`). `ALD.O` reads another invocation's output after a barrier; `ALD.P` reads the patch record. After a final barrier, invocation 0 converts the level slots to Metal's factor structures: quad = 6 halves per patch (edge[0..3] = outer levels 0..3, inside[0..1] = inner levels 0..1, `MTLQuadTessellationFactorsHalf`), triangle = 4 halves (edge[0..2], inside[0]; `MTLTriangleTessellationFactorsHalf`). A program that stores no level leaves `tess_fac` untouched, so the replay pre-fills it from the NVN default levels (`ShaderInfo.patch_slot_mask` bits 0 / 1 tell). Without a tess control shader bound the replay fills `tess_vtx` (pass-through of the vertex records) and `tess_patch` / `tess_fac` itself.

*Tess eval (`STAGE_TESS_EVAL`)*: a post-tessellation vertex function `[[patch(quad|triangle, tess_out_vertices)]] vertex VOut main0(uint patch_id [[patch_id]], float2|float3 tess_coord [[position_in_patch]], ...)` with `sp` (20), `tess_vtx` (21) and `tess_patch` (22) as `device const float4 *`; its outputs leave through the normal vertex output struct (`flip_y` / `depth_minus_one` apply). Attributes 0x2F0 / 0x2F4 are `tess_coord.x / y`, attribute 0x60 is `sp[2] + patch_id`, `ALD a[v]` reads control point v of the patch, `ALD.P` the patch record. The pipeline must be created with the same patch size (`MTLRenderPipelineDescriptor.maxTessellationFactor`, `tessellationPartitionMode` and `tessellationOutputWindingOrder` come from the NVN spacing / winding state; the translator does not see them).

**New instructions.** `OUT` (geometry only; elsewhere refused with "OUT: geometry shader output outside a geometry shader"), `VABSDIFF4`, `TXA`, `IMADSP`. Their bit layouts are the translator's reading, written down in the comments at the decoder branches; `tests/t_xlate_stages.inc` pins them, so they are *tested*, not *verified* against hardware words. `TXA` is read as `TXQ` (selector 1 = dimensions and mip count; other selectors are refused by name).

**Mac checks.** `make -C gfx test` (builds `t_xlate` with Metal: every new case runs `MSL_COMPILES`); a compile error there is a translator bug to fix in the emitted text, not in the test. **Replay (gfx-features, 2026-10-27-b6).** `gfx/src/mtl_stages.inc` (included by `mtl_replay.m`), pipelines in `mtl_shader.m` (`mtl_stage_pipe_get`, `mtl_compute_pipe_key`), tests `gfx/tests/t_stages.c`, `t_stencil_sample.c`. A draw with a geometry / tessellation program, a `PATCHES` draw, or a draw while transform feedback is capturing runs as passes: (1) *vertex capture*: the vertex program with `stage_out` (or the xfb entries) over the draw's vertex range in a 1x1 attachment-less pass, drawn as points with no fragment function (rasterization stays on: Metal requires a `void` vertex function when it is off, and the translated one returns `VOut`); `xfb_ctl = {range, lo + range * base_instance}` because `[[instance_id]]` includes the base instance. (2) *assembly*: the CPU expands the draw (indices, base vertex, primitive restart; lists, strips with odd triangles' first two vertices swapped, fans, loops, quads, adjacency, patches of `SetPatchSize`) into record ordinals and a gather kernel builds the list (`stage_in`, or `tess_vtx` without a TCS). (3) *geometry*: the GS kernel with `gs_in_vertices` from the draw mode and `gs_out_topology` / `gs_max_vertices` from the SPH (word 3 bits 24..27: 1 points, 6 line strip, 7 triangle strip; word 4 bits 0..11), then `gs_idx` drawn as restart-separated strips (points pull their record through `gs_idx`; unused slots are placed outside the clip volume) by a generated pass-through vertex function; a geometry program with no `OUT` (pass-through GS) or one that does not translate keeps the old behaviour (drawn without the geometry stage, logged once per program). *tessellation*: `tess_out_vertices` from the TCS SPH (word 2 bits 24..31) or the patch size, the TCS kernel over `npatch` threadgroups, `tess_fac` / the patch record's level slots pre-filled from `SetOuter/InnerTessellationLevels` (the TCS overwrites them when it stores levels), then `drawPatches` with the TES pipeline (`maxTessellationFactor` 64, half factors per patch, partition / winding from the parameters below). (4) *transform feedback* without a geometry / tessellation stage: a non-indexed points / lines / triangles draw whose primitives all fit is captured by the vertex program itself; any other draw captures records and a copy kernel writes the decomposed primitives in order, stopping at whole primitives when a buffer is full. Byte counts are kept per buffer at encode time; the control memory given to Begin / End / Pause / Resume / DrawTransformFeedback uses Napple's own layout (u32 bytes[4], u32 stride[4]). Driver constants of the extra stages: the storage buffer table of hardware stage n is at c0 + 0x110 + 0x100 * n (VS 0, TCS 1, TES 2, GS 3, FS 4; VS and FS measured, the others inferred from the spacing).

**Parameters the control section carries (`gfx/src/shader/control.c`, `stages.h`; gfx-stages-real, 2026-10-27-b7).** `gfx_shader_register` reads every stage's GLSLC control section (`control.h` lists the known fields: +0x20 length, +0x6fc / +0x700 / +0x704 constant and code bounds, +0x714 stage, +0x748 compute group) and registers what it finds: a transform feedback layout for a vertex / geometry / tess eval program (`gfx_xfb_set_layout`, keyed by that program's blob hash) and the tess eval parameters (`gfx_tess_set_params`). **The offsets of those two are still a hypothesis**: no program of the four titles on hand (RomFS shader archives, module GLSLC outputs and the programs created during 45 s boots scanned; Mario Odyssey creates 3 distinct geometry programs and Animal Crossing 2 at boot, Mario Kart 8 Deluxe ships one, all of which translate and compile through the geometry kernel path, but no draw binds one in 45 s; no title has a tess control / tess eval program; no vertex or geometry output carries data outside the known fields) uses them, so `GLSLC_XFB_*` (+0x30: u32 stride[4], u32 count[4], u8 locs[4][128] in the hardware's varying-table form) and `GLSLC_TESS_MODE_OFF` (+0x718, the Maxwell `TESSELLATION_MODE` form) are read only when the bytes there are non-zero and self-consistent (`t_control` pins both readers on synthetic sections and a real vertex section). Explicit registrations (`gfx_program_set_*`, keyed by the stage that feeds the rasterizer: geometry, else tess eval, else vertex) still work and overwrite. `NAPPLE_GFX_CTL_DUMP=<dir>` writes every registered control section and code area, and the boot log says once when a title creates a geometry / tess control / tess eval program. Without a layout nothing is captured (one warning per program), without parameters tessellation uses triangles / equal spacing / CCW (`NAPPLE_GFX_TESS_DOMAIN=quad` changes the domain).

**Covered since b7.** *GS instancing*: `sp[3]` = invocations (SPH word 2 bits 24..31, up to 32); the kernel runs `sp[0] * sp[3]` threads, thread = (primitive `gs_pid = gid / sp[3]`, invocation `inv = gid % sp[3]` = `SR_INVOCATION_ID`), and every thread owns its own `gs_vtx` / `gs_idx` slots (record `gid * gs_max_vertices + n`). *Triangle strips with adjacency*: assembled from the GL 4.6 table 10.1 (six vertices per triangle, odd triangles with their first two swapped). *Indirect draws with stages or during capture*: the arguments are read from guest memory at encode time (as indirect dispatches are), right when the game wrote them before the submit, wrong for arguments a GPU pass of the same submission produces (logged once); `MultiDraw*IndirectCount` with stages is still skipped. *Transform feedback of geometry output*: the vertex count a geometry program emits is only known on the GPU, so the first such capture moves the session's counts to a 32-byte counter buffer (`xfb_go_gpu`): the serial `xfbgpu` kernel decomposes `gs_idx` (restart-separated strips / points) into whole primitives, writes the layout's entries from `gs_vtx` and advances the counters; later vertex-stage captures of the same session go through records and the same kernel; End / Pause blit the counters into the control memory (so the guest reads them after the submission completes, as on the console), Resume of a control memory last written by the GPU blits it back, and `DrawTransformFeedback` of such a control memory draws indirectly (`xfbargs` kernel). Since b8 every capture keeps the program's own position values (the clip-space conversions moved to the pass-through vertex function, `stage_pipe_key.gs_clip`). The layout is looked up by the geometry program's hash, then the vertex program's. Geometry and tessellation together, transform feedback of tessellation output and isolines: see **Software tessellator (b8)** below. The primitive id runs on across instances. `NAPPLE_GFX_NO_STAGES=1` restores the old behaviour (stages ignored).

**Software tessellator (gfx-stages-complete, 2026-10-28-b8).** Metal's tessellator feeds the rasterizer only, so a `PATCHES` draw that needs more goes through a software tessellator in `mtl_stages.inc`: an **isoline** domain, a draw while transform feedback **captures tess eval output**, or a **geometry program after tessellation** (any of these; plain triangle / quad patches keep `drawPatches`). Passes 1-2 and the TCS are the same; then the `tessgen` kernel (one thread per patch) reads the factor record (quad layout for isolines: outer[0] lines, outer[1] segments) and writes a fixed number of slots per patch: coordinates `float4(u, v, w, patch id)` and index strips (3 + restart per triangle, 2 + restart per line segment, unused slots restarts; winding from the parameters). The tess eval program then runs as a **plain vertex function** over those slots (`ShaderKey.tess_soft`: `vertex VOut main0(uint vid [[vertex_id]], ..., device const float4 *tess_uv [[buffer(19)]])`, `TESS_UV_BUFFER_INDEX` reuses the `stage_in` slot), either drawn as indexed line / triangle strips (`SPK_TESS_SOFT`) or into stage-out records with rasterization off (`SPK_TESS_SOFT_CAPTURE`, `capture_void`). Capture: the `xfbgpu` kernel over the index strips (GPU-counted session, as geometry output). Geometry after tessellation: the `tesslist` kernel packs the non-empty primitives into record ordinals and writes the GS kernel's `sp` (count on the GPU), the GS output slots of threads past the count are pre-filled as restarts, and `gs_run` (shared with the plain geometry path) does the rest; its output is drawn and / or captured. Topology: isolines follow GL (lines at v = j / n0, j < n0); triangles and quads are a **uniform grid** (quad nu x nv cells from max(inner, the two parallel outer levels), triangles a barycentric grid of the largest level): the hardware's vertices when all levels are equal with integer spacing, but not its ring triangulation; fractional spacing rounds the segment count to the spacing's parity with evenly spaced vertices. Outer level <= 0 culls the patch. Level cap: exact without a TCS (the CPU knows the levels), `NAPPLE_GFX_SOFT_TESS_MAX` (default 16) with one (logged once). Translator: `tess_soft` also allows `stage_out` / xfb entries / `capture_void` on tess eval and isolines (a TCS for isolines writes the quad factor record); `TRANSLATOR_VERSION` 14. Indirect `PATCHES` draws go through the same path (arguments read at encode time, as above). Tests: `t_stages` 4a-4d (exact words of the captured vertices and the control memory, pixels), `t_xlate_stages.inc` (soft tess eval contract). Still not covered: `MultiDraw*IndirectCount` with stage programs (`do_draw_indirect_count` in `mtl_replay.m` draws them without the stages), point-mode tessellation, the exact hardware triangulation of unequal levels.

**Control-section offsets checked (b8).** `NAPPLE_GFX_CTL_DUMP` of 45 s boots (LAS, MK8D A32 and 64-bit, ACNH, SMO: 20624 sections) run through `gfx_control_parse` / `_xfb` / `_tess`: all parse, no layout or tess word is reported (no false positive). Facts now in `control.h`: section lengths are 0x7d0/0x7d1, 0x848/0x850, 0x880 and up to 0x11d0 (odd lengths and long sections are now kept, they fell back to 0x880 before); +0x030..+0x6ef is zero except in 18 sections with embedded NV assembly text (a fixed pattern at +0x034..+0x058 the consistency checks refuse); +0x718 is fragment state (1 / 0x10000) in fragment sections, so it is only a candidate for tess eval sections. No title has tess control / tess eval programs or a transform feedback layout, so the two candidate offsets remain unproven (not contradicted).

**Real titles (gfx-stages-live, 2026-11-01-r1).** `tools/gpu/scan_stages.py` now reads RomFS images (base, the patched update, DLC) with SARC / Yaz0 / zstd opened, model archives included; over the four titles on hand: geometry programs only (Odyssey 2 distinct, in `ShaderData/alThunderShader.szs`; Animal Crossing 3.0.3 8 distinct, `Swkbd/shader/*OceanShader*`, 2 created at boot; Mario Kart 8 Deluxe v1376256 1, `Common/Gfx/gsys_resource`; Link's Awakening none), no tess control / tess eval program and no transform feedback layout anywhere, so the `GLSLC_XFB_*` / `GLSLC_TESS_MODE_OFF` candidates stay unproven (table in `docs/guides/gpu-title-scan.md`). Odyssey's geometry programs belong to the stages whose data references thunder (the Odyssey travel cutscenes `DemoWorldMove*` with `RequestThunderVtxNum`, New Donk City, the Ruined Kingdom; inferred from the stage data), none reachable by a scripted 120 s run from the saves on hand (they reach Cap Kingdom / the airship intro; Animal Crossing's reach the name keyboard, which does not bind the ocean programs). **GLSLC vertex handles (translator fix, `TRANSLATOR_VERSION` 15):** GLSLC geometry code reads its inputs as `S2R SR_INVOCATION_INFO` (0x1d), handle k = (info & 0xff) * ((info >> 16) & 0xff) + k, `ISBERD`, `ALD a[handle][..]`, with the handle 0x80000000 when k >= the vertex count. SR 0x1d now answers vertex count << 16 | invocation in geometry (patch vertices << 16 in tess control / eval), `ISBERD` in those stages turns a handle into the ALD vertex operand (geometry: modulo the vertex count), and a geometry ALD whose vertex operand lies past the primitive reads 0. Before, every handle was 0x80000000 and the kernel read another record (the index wrapped): Odyssey's lightning lines all collapsed to a point. Checked by `t_stages` 2f (the prologue pattern, exact captured words) and by **`t_stages_dumped`**: `NAPPLE_GFX_STAGE_DUMP=<a NAPPLE_GFX_CTL_DUMP dir>` runs a title's real geometry programs through the replay (points / lines / triangles input, constant buffers of 1.0, output captured; skipped without the variable): Odyssey's 3 and Animal Crossing's 2 boot-time programs go through the geometry stage with finite output, Odyssey's pass-through line program reproduces each input line exactly. **Evidence lines:** `stage path ran: geometry | tessellation | software tessellation, program <hash> ...` once per program and the running count at powers of two. **`MultiDraw*IndirectCount` with stage programs:** `stage_draw_indirect_count` (`mtl_stages.inc`) reads the count and records at encode time and sends each draw through `do_draw_indirect`; it takes effect once `do_draw_indirect_count` (`mtl_replay.m`) calls it first (`t_stages` 2e, `-DNAPPLE_TEST_IND_COUNT_STAGES`).

**Shader corpus, integer textures, cube and sparse views (gfx-shader-views, 2026-11-01-r2).** Every distinct program of the four titles (base + updates + DLC; `tools/gpu/scan_stages.py extract`, guide `docs/guides/gpu-title-scan.md`) runs through `t_xlate`'s corpus mode: LAS 5767, ACNH 8577, MK8D 26544, SMO 31750 programs, all translate and compile with Metal after the fixes they found (`TRANSLATOR_VERSION` 16): `LEA.HI` constant-buffer form (opcode 000110, yuzu's layout; SMO compute), `TEX` AOFFI on 3D textures (`int3`) and none on 1D / cube, `TLDS` texture_info 6 (2D.LZ.MS), `TXQ` / `TXA` through `txq_w` / `txq_h` / `txq_n` overloads (a later TLDS.MS makes the binding multisampled), `TLD4.PTP` as four gathers (texel i = the base texel of the gather at offset i, SPIRV-Cross's lowering). **Integer sampled textures:** `ShaderKey.tex_uint_slots` / `tex_sint_slots` (depth_slots numbering) declare a binding `texture2d<uint|int>` (2D, 2D array) read through `IntTex2D` / `IntTex2DArray` (read / nearest sample return the four channels as raw integer bits, signed formats sign-extended); `mtl_tex_int_kind` gives a binding's kind, and once the replay fills the masks integer views of another class become copies (RG32UI of BC1). The masks are filled by `int_slot_masks` in `mtl_replay.m` / `mtl_stages.inc` (the hook lands with the integration; until then the declaration stays float). **Cube view of a 2D texture:** a CUBEMAP view of a square 2D texture is a copy-backed cube (`AK_CUBE`) with the 2D image on every face (ACNH binds a 15x15 R8 texture of 2.5 KiB as a cube; the console reads unrelated memory for faces 1-5); before, the shader's cube binding got a dummy texture. **Sparse residency:** `nvnMemoryPoolMapVirtual` keeps the textures of the remapped range (`mtl_tex_remap_scope`; they were forgotten and recreated, losing GPU-written tiles), a sparse texture never becomes `gpu_owned`, and a residency change re-uploads only the 64 KiB tiles whose pages changed (`upload_texture_part`; later levels whole). **Binary archive:** `serializeToURL:` writes nothing (and answers YES) when the archive gained nothing since its last serialization; that no longer disables the archive (MK8D "renaming the saved archive into place failed"), and temporary files are per process.

**Replay gaps (gfx-replay-gaps, 2026-11-01-r3).** *D24 / D24S8 readback:* a texture -> buffer copy of DEPTH24 (0x33) / DEPTH24_STENCIL8 (0x35), held as 32-bit float depth (+ stencil), blits the aspects to staging and a kernel packs NVN's words (`copy_d24_from_tex`: unorm24 in the low bits, or depth << 8 | stencil), the exact inverse of the upload (`t_d24_readback`). *Separate texture / sampler handles:* the replay keeps `nvnCommandBufferBindSeparateTexture(s)` / `Sampler(s)` per stage (`S.septex` / `S.sepsamp`); the translator turns a bindless TEX whose handle register is (constant buffer word) | (constant buffer word) (LOP.OR, IADD, LOP3 with an OR table) into `ShaderTexture.separate` with both words, and `bind_stage` ORs them, each from c2 (combined table at 0x20, separate textures at `SEP_TEX_C2_BASE` 0x120, separate samplers at `SEP_SAMP_C2_BASE` 0x520, 8 bytes per binding) or from a user uniform buffer. The c2 placement of the separate tables is a **hypothesis** (no title binds separate handles; logged once with the offsets a shader uses, for calibration) (`t_sep_sampler`). *Integer views of other targets:* `texture3d` / `texturecube` / `texture2d_ms<uint|int>` through `IntTex3D` / `IntTexCube` (face and texel from the direction) / `IntTex2DMS` (`t_int_tex` 4-6). `TRANSLATOR_VERSION` 17.

**Views and raster state (gfx-views-raster, 2026-11-02-r4).** *Stencil views of cube targets:* a stencil-mode view of a cube / cube array depth-stencil texture is declared `texturecube<uint>` / `texturecube_array<uint>` and read through `StencilTexCube` / `StencilTexCubeArray`: the direction selects the face (`cube_face`, the GL / Metal face convention) and the texel, level 0, nearest (`t_stencil_sample`). *Integer views of the remaining targets:* 1D, 1D array, cube array and 2D multisample array get `texture1d` / `texture1d_array` / `texturecube_array` / `texture2d_ms_array<uint|int>` through `IntTex1D` / `IntTex1DArray` / `IntTexCubeArray` / `IntTex2DMSArray`; only texel buffers keep the float declaration (`t_int_tex` 7-10). The wrappers' `calculate_*_lod` return a float, as Metal's do. Buffer <-> texture copies of a 1D array take the region's y / height as layers (as in GL). *Separate textures:* `ShaderKey.sep_depth_slots` / `sep_stencil_slots` / `sep_uint_slots` / `sep_sint_slots` mark separate texture binding k (c2 `SEP_TEX_C2_BASE` + 8k, k < 32) as the combined masks mark combined slots. The replay fills them from `BindSeparateTexture(s)` (`sep_slot_masks`, also for geometry / tess stages through `stage_key_masks`), `pipe_key.sep_*` carries them, and the translator applies them to a separate pair's texture word (a combined-table word takes the combined masks; a word from a user uniform buffer gets none) (`t_sep_sampler` 3-4). *Viewport swizzles:* `nvnCommandBufferSetViewportSwizzles` is recorded (`OP_VP_SWIZZLE`, four `NVNviewportSwizzle` per viewport). Viewport 0's swizzle (`ShaderKey.vp_swizzle`, `pipe_key.vp_swizzle`, XOR `VP_SWIZZLE_IDENTITY` so 0 is the identity) is applied to the clip position of the stage that feeds the rasterizer (vertex, tess eval, the geometry pass-through), after transform feedback capture and before the flip / depth conversions. Viewports 1-15 have their own (`ShaderKey.vp_swizzle_vp`, since 2026-11-02-r5): a program that writes the viewport index (vertex / tess eval attribute 0x68, or slot 0 `.z` of a geometry program's output) selects its viewport's swizzle from a 16-entry table at run time (`maxwell_vp_swizzle_msl`); capture-only passes apply none (`t_vp_swizzle`). *Provoking vertex:* `nvnCommandBufferSetProvokingVertexMode` is recorded (`OP_PROVOKING`). Metal takes flat outputs from the first vertex (checked on the M5: lists, strips, lines), which is mode 0. Mode 1 (last) re-emits direct draws as lists with each primitive's last vertex first (`pv_last_indices`: triangles rotated, which keeps the winding; lines reversed; strips, loops, fans, quads and quad strips split; polygons keep their first vertex; primitive restart honoured). Indirect draws and geometry / tess draws keep the first vertex (logged once). The 0 = first, 1 = last mapping follows the Maxwell register, because the NVN enum is not public: a **hypothesis** (`t_provoking`). New opcodes `OP_VP_SWIZZLE` / `OP_PROVOKING` were appended before `OP_COUNT` (enum only, no layout change). `TRANSLATOR_VERSION` 18.

**Viewport arrays and layered rendering (gfx-multi-viewport, 2026-11-02-r6).** *Viewports:* every draw sets all 16 viewports, depth ranges and scissors on the encoder (`setViewports:count:16`, `setScissorRects:count:16` in `apply_raster_state`), each converted as viewport 0 always was (lower-left origin, render target scale). An unset or empty viewport covers the target; an unset scissor (`S.sc_set` is a bit per scissor) covers the target. Every scissor is clamped to Metal's render pass size (`P.pw` / `P.ph`: the smallest attachment at its level), which is smaller than the guest size in a pass into a mip level (Metal's validation aborts otherwise: Animal Crossing). A vertex / tess eval program that writes the viewport index (0x68) declares `uint vpi [[viewport_array_index]]` (clamped to 15); a geometry program's index (slot 0 `.z`) goes out through the pass-through vertex function (`stage_pipe_key.gs_sys`, `GS_SYS_VP_INDEX`). *Layers:* a program that writes the layer (0x64, or slot 0 `.y` of a geometry program) declares `uint layer [[render_target_array_index]]` only when the pass is layered (`ShaderKey.layered`, `pipe_key.layered`), because Metal rejects that output in a pass without `renderTargetArrayLength`. The pass is layered on demand: the first such draw in a pass whose attachments all have two or more layers from their slice (2D array, 2D multisample array, cube, cube array; limited by the view's layer count) ends the pass and reopens it with `renderTargetArrayLength` = that count (`pass_routing`), and the pipeline is looked up again with the layered key. The layer is relative to the attachment's slice. With a single-image attachment (a 2D texture, a one-layer view, a 3D texture) the layer is dropped and the primitive lands in the attachment's own image, as in GL. Clears folded into a pass's load action still clear the attachment's own slice only (as before this change); whether NVN clears every layer of a layered target is not checked. The first draw of each kind is logged (`viewport index written (program ...)`, `layer written (program ...)`). `ShaderInfo.writes_vp_index` / `writes_layer` say what a program stores; `t_xlate` corpus mode counts them per title (`corpus <tag>: N write the viewport index, M the layer`; `NAPPLE_SHADER_CORPUS_XLATE_ONLY=1` skips the Metal compiles): none of the 72,638 programs of the four corpus titles (LAS, ACNH, MK8D, SMO) writes either (2026-11-02). The arrays are set again only when they changed in the encoder (`g_vp_serial`). Tests: `t_multi_viewport` (one draw over viewports 0 / 1 / 2 with their own scissors and depth ranges; a two-layer array split by layer; a one-layer view; a geometry program writing viewport 2 / layer 1; a tess eval program writing viewport 1 / layer 1). `TRANSLATOR_VERSION` 20.

**Rare control/system ops and stencil texture bindings (shader-rare-ops).** `BPT IDE RAM SAM STP R2B` are documented no-ops, `RTT` ends the thread, `B2R` writes 0, `PEXIT` (the next `EXIT` jumps to its target), `PLONGJMP`/`LONGJMP` (a second PBK/BRK-style stack), `GETCRSPTR`/`SETCRSPTR` (the CAL depth `cal_n`), `GETLMEMBASE`/`SETLMEMBASE` (a readable `lmb` variable), `ISBERD` (register-addressed attribute read, the ALD indexed path) and `IDP` (`idp()` helper: 4 byte or 2 half dot product + accumulate) are translated; their field layouts are the translator's reading (`tests/t_xlate_rare.inc`), no application shader emits them. **Stencil sampling:** `ShaderKey.stencil_slots` (same slot numbering as `depth_slots`, wins over it) marks bindings whose bound texture view is in stencil mode. Such a binding is declared `texture2d<uint> tN_u [[texture(N)]]` (`texture2d_array<uint>` for arrays; other targets and depth comparison are refused) and wrapped in a local `StencilTex2D`/`StencilTex2DArray` named `tN`, whose `sample`/`gather`/`read` return `(stencil, 0, 0, 1)` as raw integer bits (level 0, nearest). The Mac replay side must: set the `stencil_slots` bit when `mtl_tex_view` has `ds_mode` stencil for that binding slot (as `depth_slot_mask` does for depth), bind the X32_Stencil8 / Stencil8 view (pixel format uint) at texture index N, copy `stencil_slots` into the per-stage `ShaderKey` in `mtl_shader.m` and include it in the pipeline key (`TRANSLATOR_VERSION` 12).

### 14. Guest object sizes (2026-11-01)

NVN objects live in guest memory, and a title allocates each one with its own SDK's `sizeof`. That size can be smaller than the reserved size in `gfx/src/nvnapi/nvn.h`. A host layout (`gfx/src/obj.h`) that is larger than the title's object writes over whatever the title placed next to it. Animal Crossing's "program hash 0x3" was such a case (30e67b4): its NVNprogram is followed by an NVNbuffer at +0x80.

`NVN_MIN_<type>` in `obj.h` is the smallest size measured across the four test titles: SMO (SDK 3.5.1), MK8D (0.12.16, 32-bit), LAS (8.2.0) and ACNH (20.5.4). `FITS(T, H)` asserts `sizeof(T) <= NVN_MIN_H <= sizeof(H)` for every layout the API writes into a guest object. `gfx/tests/t_obj_canary.c` places a canary right after `NVN_MIN_<type>` bytes of a block, runs SetDefaults or Initialize and every entry point that takes the object as a non-const first parameter (328 calls, Finalize last), and fails on the first call that writes into the canary. Host-only state that does not fit stays in side tables keyed by the guest address: program stage hashes and sample shading (`api_program.c`).

**Evidence.** Running with `NAPPLE_GFX_OBJTRACE=1` (`gfx/src/api_objtrace.c`) records the address of every object at Initialize, Finalize and SetDefaults. For objects with a lifetime, it keeps an address-ordered set of live objects, and the gap from an object to the next live NVN object above it is an upper bound of its size in that title. In arrays, that gap is the exact size.

The tracker leaves objects out of these gaps in three cases:
- **On the calling stack**: the object lies just above the gfx frame. 64-bit titles run on the guest's stack, so such an object is a temporary that is never finalized.
- **Set up again without Finalize**.
- **Freed without Finalize**: the object's first word changed since it was initialized, which means the guest freed and reused its memory.

Builders, texture views and state objects have no lifetime. They are paired with the objects the same thread set up in its last 12 NVN calls, which is weak evidence because two calls can reuse the same stack.

A neighbour inside nvn.h's reserved size is logged once per type and gap as `objtrace close:`. Run `NAPPLE_GFX_OBJTRACE=1` with a 90 s headless boot; the per-type summary goes to stderr at exit and every 20 s.

Rule: `NVN_MIN = min(nvn.h reserved size, smallest credible gap in any title)`.

| Object | nvn.h | ACNH 20.5.4 | MK8D 0.12.16 (a32) | SMO 3.5.1 | LAS 8.2.0 | NVN_MIN | host layout |
|---|---|---|---|---|---|---|---|
| NVNdevice | 0x3000 | 0x3078 (texture pool) | 0x3040 (texture pool) | 0x3078 (texture pool) | 0x3020 (queue) | 0x3000 | 0x20 |
| NVNqueue | 0x2000 | 0x20a0 (pool) | 0x2060 (pool) | 0x20a0 (pool) | 0x2028 (window) | 0x2000 | 0x20 |
| NVNsync | 0x40 | 0x60 (window) | 0x50 (command buffer) | 0x60 (command buffer) | 0x58 (sync) x5 | 0x40 | 0x18 |
| NVNevent | 0x40 | not used | not used | not used | not used | 0x40 | 0x20 |
| NVNmemoryPool | 0x100 | **0x100** (buffer) x17 | **0x100** x12 | **0x100** x28 | 0x120 x2 | 0x100 | 0x40 |
| NVNbuffer | 0x30 | **0x30** (pool) x24 | **0x30** (buffer) x3 | **0x30** (pool) x24 | 0x48 (buffer) x50 | 0x30 | 0x20 |
| NVNtexture | 0x80 | 0x88 (pool) x22498 | no credible reading (1) | 0xe0 (texture) x3 | 0xe0 (texture) x619 | 0x80 | 0x58 |
| NVNsampler | 0x30 | 0x78 (sampler) x8691 | 0x78 x2214 | 0x78 x376 | 0x78 x36 | 0x30 | 0x30 |
| NVNtexturePool | 0x20 | **0x20** (sampler pool) | **0x20** (sampler pool) | **0x20** (sampler pool) | 0x138 | 0x20 | 0x20 |
| NVNsamplerPool | 0x20 | **0x20** (sampler) | 0x3140 | **0x20** (sampler) | 0x1430 | 0x20 | 0x20 |
| NVNprogram | 0xC0 | **0x80** (buffer, program alive) x2 (2) | 0x100 (program) x3661 | 0xc0 (buffer) x1 | 0x100 (program) x133 | **0x80** | 0x28 |
| NVNwindow | 0x180 | 0x1a0 | 0x190 | 0x1a0 | 0x1a0 | 0x180 | 0xf8 |
| NVNcommandBuffer | 0xA0 | 0xc0 (pool) | 0xb0 (pool) | 0xc0 (pool) | 0x140 (command buffer) x19 | 0xA0 | 0x30 |
| NVNvertexAttribState | 0x4 | **0x4** array x370179 | **0x4** x93678 | - | **0x4** x56 | 0x4 | 0x4 |
| NVNvertexStreamState | 0x8 | **0x8** array x191214 | **0x8** x71962 | - | **0x8** (to attrib) x148 | 0x8 | 0x8 |
| NVNcolorState / NVNchannelMaskState / NVNpolygonState | 0x4 | **0x4** (next state) | **0x4** | - | **0x4** | 0x4 | 0x4 |
| NVNblendState / NVNdepthStencilState | 0x8 | weak | weak | weak | weak | 0x8 | 0x8 |
| NVNmultisampleState | 0x18 | 0x30 | 0x2c | - | 0x2c | 0x18 | 0x13 |
| NVNtextureView | 0x10 | weak (3) | weak | weak | weak | 0x10 | 0x10 |
| builders: device / queue / memory pool / buffer / texture / sampler / window / event | 0x40 / 0x40 / 0x40 / 0x40 / 0x80 / 0x60 / 0x40 / 0x20 | weak | weak | weak | weak | as nvn.h | 0x4 / 0x40 / 0x20 / 0x20 / 0x60 / 0x38 / 0x40 / 0x10 |

Gaps are hex bytes, with the neighbour's type and how often each gap was seen. Bold marks a gap equal to the minimum, i.e. the exact size. "-" means no reading for that title. Logs are in `NappleWork/logs/objlay-{acnh,mk8d,las}-trace2` and `objlay-smo-trace3`.

1. MK8D runs as a 32-bit title, so its guest stack is not the host stack and its stack temporaries cannot be recognised. It sets up about 219,000 textures in 90 s, and its smallest texture gaps (0x8, 0x10, 0x40 and so on, 1 to 4 times each) are dead temporaries; the first words logged for most of them are not a texture's.
2. The ACNH program at 0x7037b845c0 still carried its magic when the game set up an NVNbuffer at +0x80 and a memory pool at +0xb0, which suggests a structure {NVNprogram, 0x80 bytes; NVNbuffer, 0x30; NVNmemoryPool}. The r2 layout still zeroed +0x80..0x88 at Initialize and SetShaders, which is the start of that buffer. The program's guest-visible part is now 0x28 bytes.
3. Builders, views and most state objects are stack locals, and the "recent objects" pairing mixes frames of different calls. For example, a texture view 8 bytes from another view cannot be a real size, because a view's levels, layers, format, swizzle and target do not fit in 8 bytes. For these objects only array strides count as evidence (vertex attribute and stream states). The other minimums are nvn.h's, and no title contradicts them.
