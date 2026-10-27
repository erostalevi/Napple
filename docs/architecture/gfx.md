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
