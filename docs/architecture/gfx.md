# Graphics: libnapple_gfx

`libnapple_gfx` implements the NVN API on Metal. The guest's own NVN driver never runs: the runtime redirects `nvnBootstrapLoader` to gfx, gfx records
the guest's command buffers, translates Maxwell shader programs to Metal Shading Language in process, and replays the recordings on a Metal queue.
This page describes the design, the interface between the runtime and gfx, and the parts of the replay that need more than a direct Metal mapping.
The authoritative interface is the header `gfx/include/napple_gfx.h` (pure C); the history of changes is in the [change archive](../archive/changes/README.md).
Per-title graphics findings are in the [title notes](../titles/README.md).

## Design decisions

### Shader translation: a native in-process Maxwell to MSL translator
Options evaluated on real shader corpora (BNSH programs extracted from game archives):

| Option | Result |
|---|---|
| (a) Ryujinx translator + SPIRV-Cross via a NativeAOT service | Not used: needs `dotnet` to rebuild or patch, the prebuilt binary is tuned to one game's driver-constant layout, needs a helper process (impossible on iOS), adds a SPIR-V hop. Kept only as an offline reference. |
| (b) native C translator from LASRecomp (MIT) | Translates almost every program of the sampled corpora and the results compile with `metal -std=metal3.2`. In-process, no process spawn, about 1 ms per shader. Chosen. |
| (c) a new translator | Not needed. |

The imported translator was cleaned up (Vulkan GLSL path, per-game tint, debug hooks and mirror mode removed). `napple_gfx_config.shader_translator` is reserved and must be NULL.
Regression check: the MSL output of extracted corpora compared against a stored baseline, and the corpus mode of `t_xlate` (see Tests).

Control section facts used by the translator: +0x700 code length (constants start there), +0x6FC constants size, +0x714 stage, compute local size at
+0x748/+0x74C/+0x750. Texture handles are read from c2: combined handles at 0x20 + 8*slot.

### Object model
Textures are their hardware descriptor (storage address, format, size, layout): NVNtexture objects, texture-pool entries and recorded render targets carry the
same snapshot and the Metal layer maps equal snapshots to one MTLTexture. Programs are shader-blob content hashes. Command lists are immutable, reference-counted
records published at EndRecording; uniform buffers are snapshotted per draw at replay (UpdateUniformBuffer patches memory in replay order).
Texture upload: block-linear to linear on the CPU, dirty pages found by write protection (`src/dirty.c`; write-protect first, then read, so no write is lost).

### Command-list lifetime
A recorded command list stays valid as long as the command memory it was recorded into, as on hardware where a command handle is the address of the
recorded commands. It ends when the game finalizes the handle, when another list is recorded at the same command-memory address, or when the pool holding
that memory is finalized. It does not end with the command buffer that recorded it, nor by idle time or a memory budget: games finalize scratch command
buffers and keep replaying their lists for minutes, and dropping such a list makes a model draw without its setup ("exploded" geometry). Idle lists are held
LZ4-packed (`NAPPLE_REC_NO_PACK=1` turns packing off). `NAPPLE_REC_BUDGET_MB` (default: a quarter of physical memory) is only a safety ceiling for a runaway
game: past it the least recently used lists are dropped with a warning, which is a bug to investigate.

Recording is thread-safe: host parts of a recording are addressed by slot and generation and never freed, the recording state is locked, grown buffers
are freed late, and commands recorded outside BeginRecording/EndRecording go to a payload-sized sink. NVN command buffers are not thread-safe on hardware,
but a guest race can still make two threads record into one buffer; `gfx/tests/t_cmdshare.c` reproduces that misuse without a game, and
`NAPPLE_GFX_CBTRACE=1` dumps both threads' events and backtraces.

### Frame pacing
Guest vsync is virtual: the runtime fires it every `napple_gfx_vsync_period_ns()` (1/60 s by default), independent of the monitor refresh. Which finished
frames are shown is a host property (the present rate); presents are held with `presentAfterMinimumDuration` so a 30 fps game on a 120 Hz display is not
sped up. Headless runs pace presents at the swap interval so the guest runs at real speed. The three rates (game clock, speed, present rate) are described
in [runtime services](runtime-services.md).

## Interface (what the runtime provides and calls)

Library: `gfx/libnapple_gfx.a` (`make -C gfx`). `make -C gfx lib adapter` also builds `gfx/build/obj/adapter/gfx_napple.o`, which implements
`runtime/src/gfx.h` on top of `napple_gfx.h` (hooks mapped: `core_away` token, `wrap_guest_callable`, `on_present`); `make -C runtime GFX=napple` links them
instead of `gfx_stub.c`. The adapter builds its override table lazily, because the runtime registers overrides before `gfx_init`.

### Lifecycle

```
runtime start
  napple_gfx_config_default(&cfg)      fill: metal_layer (or NULL headless), title_id, cache_dir, hooks, settings
  napple_gfx_init(&cfg)                creates MTLDevice/queue + gfx threads; 0 = ok
  napple_gfx_hle_table(&n)             register each {symbol, fn} as a host override of the guest import of that name
guest runs: nnMain -> nn::gfx -> nv::SetGraphicsAllocator / nv::InitializeGraphics / nvnBootstrapLoader("nvnDeviceGetProcAddress") ...
every guest present -> hooks.on_present(frame, swap_interval) -> runtime signals vsync event at napple_gfx_vsync_period_ns()
exit: napple_gfx_diag_dump(); napple_gfx_shutdown()
```

### Symbols the runtime redirects

`napple_gfx_hle_table()` returns the authoritative list.

| Guest symbol | Meaning |
|---|---|
| `nvnBootstrapLoader` | returns the NVN entry points; the SDK's own NVN driver is never executed |
| (not overridden) `nv::InitializeGraphics`, `nv::SetGraphicsAllocator`, `nv::SetGraphicsDevtoolsAllocator` | run natively; overriding the allocators breaks games, and InitializeGraphics needs the runtime's nvdrv (see [runtime services](runtime-services.md)) |

Every function pointer gfx returns to the guest goes through `hooks.wrap_guest_callable(user, fn, name)` (it runs NVN calls on the guest thread's host stack,
as `runtime_hle_wrap_named` does). The guest calls NVN entry points with the plain AAPCS64 ABI: no stack arguments, up to 8 integer and 8 FP register
arguments. `NVNboolean` parameters are not extended by Switch callers: gfx masks them to 8 bits.

NVN API version: the game checks `nvnDeviceGetInteger(API_MAJOR/MINOR)` against the version it was built with and silently skips graphics initialisation on a
mismatch, so the runtime puts the game's version in `nvn_major/nvn_minor` (from `game.json`, or detected by `main.c: detect_nvn_version`; 0,0 = 55.15).

### Guest memory: what gfx needs from the runtime

1. **Identity mapping.** A GPU address passed by the game is a host virtual address. NVN memory pools wrap game memory
   (`nvnMemoryPoolInitialize(storage, size)`); gfx wraps the same pages in a zero-copy `MTLBuffer` (`newBufferWithBytesNoCopy`), so the pages must be
   page-aligned `mmap` memory owned by the process (16 KiB pages on Apple Silicon; the guest sees 4 KiB granularity, gfx rounds outward to 16 KiB).
2. **Virtual memory pools** (`NVN_MEMORY_POOL_FLAGS_VIRTUAL`, `nvnMemoryPoolMapVirtual`): gfx remaps physical pool pages (`mach_vm_remap`), so the virtual
   address range must be reserved by the guest/runtime as normal anonymous memory.
3. **Write-fault notifications (dirty tracking).** Pool pages that hold textures are marked read-only after upload; a guest write faults, the page is marked
   dirty and unprotected. Therefore:
   - the runtime's fault handler (`runtime/src/signals.c`) calls `napple_gfx_handle_fault(addr, is_write, ucontext)` first for every access fault inside
     guest memory; on `1` it retries the instruction;
   - the runtime calls `napple_gfx_guest_write_prepare(addr, len)` before any host-side write into guest memory that a syscall performs on behalf of the
     guest (file reads, IPC out-buffers, decoders): a kernel `read()` into a protected page returns `EFAULT` instead of faulting through the handler;
   - the runtime never `mprotect`s ranges handed to NVN memory pools.
4. **Shader global memory** (LDG/STG, used by SSBO access) is addressed through one buffer over the guest heap: the runtime sets `gmem_base`/`gmem_size`
   (page aligned; 0 = none, and SSBO-heavy compute shaders are then skipped).
5. **No JIT and no x18 assumptions in gfx.** gfx is plain host code and never touches guest registers.

Host API for decoders: `napple_gfx_texture_info`, `napple_gfx_texture_write_rgba` (block-linear swizzle with dirty tracking).

### Threading

- The guest's graphics threads call NVN entry points directly. gfx implements them on that thread, records, and hands work to a private translation and
  replay thread (`submit` returns before the GPU work is built).
- Blocking entries use `hooks.core_away`/`core_back`.
- gfx never calls back into guest code except through addresses the game passed itself (debug callbacks, allocators): `nv::SetGraphicsAllocator`
  callbacks are stored and may be invoked from the guest thread that is inside the NVN call.

### Presentation, resolution and pacing settings

- `metal_layer`: a `CAMetalLayer*`. gfx sets pixel format, `framebufferOnly`, `displaySyncEnabled`, drawable size. NULL selects headless mode
  (offscreen texture, same code path, used by tests and `napple run --headless`).
- `settings.output_width/height`, `internal_res_scale`, `upscaler`: see [Render scale, output fit and upscaling](#render-scale-output-fit-and-upscaling).
- `settings.pacing`: `NAPPLE_PACING_GAME` paces to `swap_interval/60 s`, `NAPPLE_PACING_FIXED` to `target_fps`, `NAPPLE_PACING_UNCAPPED`.
  `game_fps_divisor_fix=1` keeps the guest's logical step at 2 vsyncs for a game at swap interval 2 while gfx presents duplicated or interpolated frames
  at the higher rate. `present_interval_override` (0 = honour the game) treats every present as interval N.
- Window origin: "lower-left" (the NVN default) changes only window coordinates (viewport, scissor, fragment coordinate y = H - y_top via the rscale
  table); NDC +y renders to the top row in both modes. There is no vertex-shader Y negation.
- `napple_gfx_screenshot(path)`: PNG of the next presented frame, for headless verification.

### Diagnostics

`napple_gfx_diag_unimplemented()` returns the distinct unimplemented NVN entry points the guest called, with counts; each is logged once by name at WARN.
`settings.unimplemented_abort=1` aborts on the first one (CI). `napple_gfx_diag_unknown_lookups()` lists names asked of `nvnDeviceGetProcAddress` that this
build does not know. `napple_gfx_get_stats()` gives frames, draws, pipelines compiled, shader cache hits, pending async compiles.

## Guest object sizes

NVN objects live in guest memory, and a title allocates each one with its own SDK's `sizeof`, which can be smaller than the reserved size in
`gfx/src/nvnapi/nvn.h`. A host layout (`gfx/src/obj.h`) larger than the title's object writes over whatever the title placed next to it.

`NVN_MIN_<type>` in `obj.h` is the smallest size measured across the reference titles. `FITS(T, H)` asserts `sizeof(T) <= NVN_MIN_H <= sizeof(H)` for
every layout the API writes into a guest object. `gfx/tests/t_obj_canary.c` places a canary right after `NVN_MIN_<type>` bytes of a block, runs SetDefaults
or Initialize and every entry point that takes the object as a non-const first parameter (328 calls, Finalize last), and fails on the first call that writes
into the canary. Host-only state that does not fit stays in side tables keyed by the guest address (program stage hashes and sample shading, `api_program.c`).

**Measuring.** `NAPPLE_GFX_OBJTRACE=1` (`gfx/src/api_objtrace.c`) records the address of every object at Initialize, Finalize and SetDefaults. For
objects with a lifetime it keeps an address-ordered set of live objects; the gap from an object to the next live NVN object above it is an upper bound of
its size in that title, and in arrays it is the exact size. Objects are left out of these gaps when they are on the calling stack (just above the gfx
frame: a temporary never finalized), set up again without Finalize, or freed without Finalize (the first word changed since Initialize). Builders, texture
views and state objects have no lifetime; they are paired with the objects the same thread set up in its last 12 NVN calls, which is weak evidence because
two calls can reuse the same stack, so for them only array strides count. A neighbour inside nvn.h's reserved size is logged once per type and gap as
`objtrace close:`. Run it with a 90 s headless boot; the per-type summary goes to stderr at exit and every 20 s. For a 32-bit title the guest stack is not
the host stack, so stack temporaries cannot be recognised.

Rule: `NVN_MIN = min(nvn.h reserved size, smallest credible gap in any title)`. The per-title measurements are in the [title notes](../titles/README.md).

| Object | nvn.h | NVN_MIN | host layout |
|---|---|---|---|
| NVNdevice | 0x3000 | 0x3000 | 0x20 |
| NVNqueue | 0x2000 | 0x2000 | 0x20 |
| NVNsync | 0x40 | 0x40 | 0x18 |
| NVNevent | 0x40 | 0x40 | 0x20 |
| NVNmemoryPool | 0x100 | 0x100 | 0x40 |
| NVNbuffer | 0x30 | 0x30 | 0x20 |
| NVNtexture | 0x80 | 0x80 | 0x58 |
| NVNsampler | 0x30 | 0x30 | 0x30 |
| NVNtexturePool | 0x20 | 0x20 | 0x20 |
| NVNsamplerPool | 0x20 | 0x20 | 0x20 |
| NVNprogram | 0xC0 | **0x80** | 0x28 |
| NVNwindow | 0x180 | 0x180 | 0xf8 |
| NVNcommandBuffer | 0xA0 | 0xA0 | 0x30 |
| NVNvertexAttribState | 0x4 | 0x4 | 0x4 |
| NVNvertexStreamState | 0x8 | 0x8 | 0x8 |
| NVNcolorState / NVNchannelMaskState / NVNpolygonState | 0x4 | 0x4 | 0x4 |
| NVNblendState / NVNdepthStencilState | 0x8 | 0x8 | 0x8 |
| NVNmultisampleState | 0x18 | 0x18 | 0x13 |
| NVNtextureView | 0x10 | 0x10 | 0x10 |
| builders: device / queue / memory pool / buffer / texture / sampler / window / event | 0x40 / 0x40 / 0x40 / 0x40 / 0x80 / 0x60 / 0x40 / 0x20 | as nvn.h | 0x4 / 0x40 / 0x20 / 0x20 / 0x60 / 0x38 / 0x40 / 0x10 |

NVNprogram is the one object a title allocates smaller than nvn.h reserves; its guest-visible host part is 0x28 bytes and nothing is written past it at
Initialize or SetShaders.

## Pipelines, caches and the binary archive

- Translation and Metal compilation run on background queues. A draw whose pipeline is still compiling waits for it (a skipped draw is a visible pop);
  `NAPPLE_GFX_PIPE_SKIP=1` skips instead, for comparison. A draw that needs a pipeline now builds it on its own thread rather than queueing behind
  background work.
- Every pipeline key a game used is persisted (`<cache>/pipelines.bin`) and rebuilt in the background as soon as the programs it names are registered
  ("prewarm"; `NAPPLE_GFX_NO_PREWARM=1` off). Translated MSL stays on disk as text (`<cache>/shaders/<hash>.msl`; `NAPPLE_SAVE_MSL=1` adds `// pc` annotations).
- Compiled pipelines go into a Metal binary archive `<cache>/pipelines-v<TRANSLATOR_VERSION>.metalar`. Every build looks it up first; every pipeline the
  run builds is added to a fresh archive that replaces the file (at most every 10 s and at shutdown, temporary files per process). A large file loads in the
  background while draws compile meanwhile. `NAPPLE_GFX_ARCHIVE_MAX_MB` (default 64) bounds the file: when a save is bigger, the oldest quarter of the
  pipelines is left out. When Metal drops a fragment stage and the archiver refuses the whole archive, that pipeline is left out and the archive rebuilt.
  `serializeToURL:` writing nothing because the archive gained nothing does not disable the archive. A stale file (other OS or GPU) fails to load and is
  recreated. `NAPPLE_GFX_NO_ARCHIVE=1` disables the archive.
- `TRANSLATOR_VERSION` (currently 20, `gfx/src/mtl_shader.m`) is bumped whenever the generated MSL changes; it invalidates both caches.
- `NAPPLE_GFX_SPECULATE` enables shader speculation (off by default, because titles can register thousands of programs at once).

## Render scale, output fit and upscaling

The player-facing "Video" settings (overlay tab, `settings.json`, `napple_display_settings` in `gfx/include/napple_display.h`) are applied live: a change
reaches the next frame, and the game's screen-sized render targets are rebuilt after the window size was stable for 300 ms.

**Vocabulary.** *base size* = what the game believes its screen is (1920x1080 docked, 1280x720 handheld, the window's aspect when "match window aspect" is
on); *render size* = base x scale, the real size of the screen-sized targets; *output size* = the drawable.

**Render scale.** `render_mode` 0 native, 1 scale factor, 2 follow the window, 3 fixed size. The factor is free between 10 % and 800 %
(`internal_scale_percent`; presets 0.5x / 1x / 2x / 4x and "4K" / "1440p" targets in the overlay, a text field for any value). `supersample_percent`
(100..400) multiplies it and is filtered down again by the output resamplers (box / bicubic / Lanczos kernels stretched by the down-scale ratio).
`mtl_scale.m` allocates the screen-sized targets at `w * sx, h * sy`; viewports, scissors, texel fetches, size queries and the fragment position go
through the `rscale` table the shader translator reads. Which textures count as screen sized: `mtl_scale_pick_size` (same shape as the swapchain, not
smaller than base / 8, not larger than the screen, plain 2D, not mipped, not video decode / linear / sparse, rendered to by the GPU or depth).
`NVN_TEXTURE_FLAGS_IMAGE` is not a veto for GPU-written targets (titles set it on screen-sized targets). Passes that mix attachments of different sizes
are unified (`pass_unify_scale`), except mipmapped passes. `NAPPLE_SCALE_DIAG=1` logs every distinct target and the decision once (`scale-diag:` lines).

**Budget.** `napple_display_budget_pixels(working set)` (about 48 Mpixel, `recommendedMaxWorkingSetSize / 220` on small GPUs).
`napple_display_compute_scale` gives up supersampling first (down to 1x), then reduces the render scale uniformly, and never exceeds 16384 per side: 4x
with 4x supersampling degrades to about 2.7x instead of failing. `g_scaled_bytes` and the cache trim see the larger targets.

**Output fit** (`fit`, from the overlay "Window" setting): letterbox / pillarbox (bars), stretch, fill (crop the long axis, centred) and whole-number
multiple with bars. `napple_display_fit_rect` is a pure function (unit tested for 16:9, 21:9, 16:10, 4:3 and 9:16 targets). "Match window aspect"
additionally tells the game a different screen size through vi/applet (`napple_display_game_size`); a title whose HUD or camera breaks with it is marked
`"report_aspect": false` in the per-title table (it is told the native size, the picture is still fitted).

**Upscalers** (`upscaler`): 0 none, 1 MetalFX spatial, 2 built-in edge adaptive (`mtl_upscale.m`: structure tensor driven anisotropic Lanczos-2 with an
anti-ringing clamp, then a limited-lobe sharpening pass; original code), 3 CAS (Catmull-Rom plus contrast adaptive sharpening). With an upscaler the
render size is window / `upscale_factor` (1.5) and the upscaler brings it up. `napple_display_effective_upscaler` answers what is really used: MetalFX absent
(old OS, simulator SDK) falls back to 2 and the overlay says so. MetalFX temporal needs depth and motion vectors from the game, which titles do not provide,
so it is not offered. `sharpness` 0..1 scales the sharpening of 2 and 3.

**Per-title table.** `mtl_scale_table.m`, format in its header: `{"titles": {"<title id>": {"disable", "max_scale", "min_div", "skip": [[w,h]],
"force": [[w,h]], "report_aspect"}}}`; built-in defaults plus an optional file named by `NAPPLE_RSCALE_TABLE`. Mods ship the same format.

**Frame generation.** `mtl_interp.m` can fill in-between images when the present rate is above the game's frame rate.

## Surface images (SULD, SUST, SUATOM, SURED)

The translator turns the Maxwell surface instructions into Metal texture `read` / `write` / `atomic_fetch_*` calls (`gfx/src/shader/surface.inc`). The
instruction field positions follow the public Maxwell decoders; tests: `sh gfx/tests/t_xlate_linux.sh` (text assertions on hand-assembled words) and
`gfx/tests/t_image.c` (a fragment SUST.D.32 into an R32UI image read back exactly).

**Binding contract (translator output to Metal runtime).** `ShaderInfo.nimg` / `img[i]` (`shader/maxwell.h`, `ShaderImage`) list the images in order of
first use, at most `MAX_IMAGES` (8).

| Item | Rule |
|---|---|
| Metal texture index | image slot `i` is `[[texture(IMAGE_TEXTURE_BASE + i)]]`, base 64. Sampled textures use 0..62, so the two never collide. No sampler. Declared after the sampled textures, in all stages (compute, fragment, vertex). |
| MSL argument | `texture<dim><T, access::A> im<i>`; `dim` from `ShaderImage.type`: 1D `texture1d`, 1D array `texture1d_array`, 2D `texture2d`, 2D array `texture2d_array`, 3D `texture3d`, buffer `texture_buffer`. |
| Component type `T` | `float` by default. `int` / `uint` when the pipeline key says so (`ShaderKey.image_int_mask` / `image_uint_mask`, bit n = SULD/SUST immediate index n, bit 31 = every bindless image), or when the shader forces it (raw `.U8/.S8/.U16/.S16` access, atomics): then `ShaderImage.kind` is `IMGKIND_INT` / `IMGKIND_UINT` and the runtime must supply a matching format (R8Uint ... RGBA32Uint, R32Uint / R32Sint for atomics). A key that contradicts the shader is a translator error naming the instruction. |
| Access `A` | `read`, `write` or `read_write` from `ShaderImage.access` (`IMGACC_READ`, `IMGACC_WRITE`): the union of the instructions that use the image. The texture is created with the matching usage (`MTLTextureUsageShaderRead`, `ShaderWrite`; atomics also `MTLTextureUsageShaderAtomic`), and `read_write` needs `MTLReadWriteTextureTier` 1 (R32 formats) or 2 (all). |
| Handle | `ShaderImage.handle_offset`: byte offset in c2 of the NVN image handle (immediate index * 4), exactly like `ShaderTexture.handle_offset`. A bindless handle (`.B` forms, `ShaderImage.bindless`) is read from user uniform buffer `bindless_cbuf` at `bindless_offset`; a bindless handle that comes from c2 is an ordinary table entry. |
| Language version | `ShaderInfo.needs_msl31`: the MSL contains texture atomics and is compiled with `MTLLanguageVersion3_1` (macOS 14+). |
| Out of range | per instruction clamp mode: IGN and TRAP skip the access (a load returns 0, a store is dropped; there is no trap on a GPU), ZERO the same, CLAMP clamps each coordinate and the layer to the extent. The checks are explicit, not left to Metal. |

**What is translated.**
- SULD / SUST, dimensions 1D, BUFFER, 1D_ARRAY, 2D, 2D_ARRAY, 3D (coordinates in consecutive registers from Ra: x, y, z, and the array layer last). `.D`
  raw sizes U8 S8 U16 S16 32 64 128 (1, 1, 1, 1, 1, 2, 4 registers; sub-word sizes need an integer image), `.P` formatted access with a channel mask
  (channels compact into consecutive registers; a partial-mask store is a read-modify-write and makes the image `read_write`), `.BA` byte addressing (x is
  divided by the access size, which assumes the texel size equals the access size).
- SUATOM (returns the old value) and SURED (no result, data in Rd): ADD, MIN, MAX, AND, OR, XOR on 32-bit signed / unsigned integer images. MIN / MAX use
  the signedness of the instruction type, and the image is declared `int` or `uint` accordingly.
- Refused, each a translator error naming the instruction (for example `SUATOM.EXCH.2D: ...`): INC and DEC (no Metal texture atomic), EXCH and CAS (MSL 3.1
  texture atomics are fetch_add/sub/and/or/xor/min/max only), 64-bit and floating-point atomics, more than 8 distinct images, one image used with two
  dimensions, other opcodes of the 0xea / 0xeb range.

**Known approximations.** A raw `.D` access without `.BA` returns the first channel(s) of the texel, not the packed texel bits; formatted UNORM/SNORM data
comes back as Metal's converted float, the same value `.P` returns on hardware. Float-image `.D.32` moves the bit pattern through the float channel (NaN
payloads may be canonicalised by the texture unit).

**Replay side** (`mtl_replay.m` `bind_stage` image loop and `image_masks`, `mtl_tex.m` `mtl_tex_image_view`, `mtl_shader.m` key masks and `compile_options_v`):
1. After `maxwell_translate`, each `info->img[i]` texture is bound with `setTexture:atIndex:IMAGE_TEXTURE_BASE + i` on the compute / fragment / vertex
   encoder; the handle is resolved from c2 at `handle_offset` (or the bound user buffer at `bindless_cbuf` / `bindless_offset`) through the same TIC lookup
   as sampled textures.
2. The texture is a view with `MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite` (plus `ShaderAtomic` when `img[i].atomic`) and an integer pixel
   format when `kind` is `INT` / `UINT`; sRGB views cannot be written, the linear twin is used.
3. `ShaderKey.image_int_mask` / `image_uint_mask` come from the formats of the bound images (the same shader with different formats is a different pipeline).
4. Compiled with `MTLLanguageVersion3_1` when `needs_msl31`. `useResource:usage:` is not needed (no argument buffers).
5. A shader-written image makes the Metal texture the owner of the data (`GfxTex.gpu_owned`, as for render targets): CPU uploads stop, reads go to the GPU
   copy; there is no write-back into guest pages.

## Geometry, tessellation and transform feedback

Metal has no geometry stage and no transform feedback, and its tessellation is split differently from Maxwell's. The translator
(`gfx/src/shader/maxwell_translate.c`) therefore emits compute kernels and vertex functions that read and write plain buffers, and the replay
(`gfx/src/mtl_stages.inc`, included by `mtl_replay.m`; pipelines in `mtl_shader.m` `mtl_stage_pipe_get`, `mtl_compute_pipe_key`) wires the passes together.
All buffer indices and record layouts are macros in `shader/maxwell.h`; the key fields that drive them are in `ShaderKey`. `NAPPLE_GFX_NO_STAGES=1`
ignores the stages (draws without them).

### Stage records
Everything that travels between stages is an array of records of `STAGE_SLOTS` (34) `float4`: slot 0 = system values (attribute 0x60..0x6F: x primitive
id, y layer, z viewport index, w point size), slot 1 = position (0x70), slot `2 + n` = generic attribute n (0x80 + 16 n). `ShaderInfo.stage_slot_mask`
says which slots a program stores; the others may be left unwritten. Patch records (tessellation) have the same size: slot 0 = outer levels (0x00..0x0C),
slot 1 = inner levels (0x10, 0x14), slot `2 + n` = generic patch attribute n (0x20 + 16 n); `ShaderInfo.patch_slot_mask`.

### Vertex capture
`ShaderKey.stage_out` makes the vertex function also write its record to `device float4 *stage_out [[buffer(29)]]` at `xo * STAGE_SLOTS`; `ShaderKey.xfb[]`
(`xfb_count` entries, `xfb_stride[k]` bytes per vertex) writes the named components to `device uint *xfb<k> [[buffer(24 + k)]]` (only the buffers in
`ShaderInfo.xfb_buffer_mask` are declared). Both use `constant uint *xfb_ctl [[buffer(28)]]`: `[0]` vertices per instance, `[1]` first vertex, `[2 + k]`
start of xfb buffer k in 32-bit words (the NVN buffer offset plus the counter, divided by 4). The vertex ordinal is
`xo = xfb_ctl[0] * instance_id + (vertex_id - xfb_ctl[1])`, so these passes draw non-indexed (an indexed draw is expanded into a vertex sequence first, as
the hardware captures in processing order). The values are the shader's own outputs, taken before the `flip_y` / `depth_minus_one` conversions. An entry's
`addr` is the Maxwell attribute address of its first component (0x70 position, 0x80 + 16 n generic, 0x60..0x6F system values); an output the program never
wrote captures as 0. Fragment, compute, geometry and tess-eval keys carrying `xfb_count` / `stage_out` are refused with a named error (geometry output is
captured from `gs_vtx` / `gs_idx` by a copy pass; tess-eval output only exists after tessellation).

### Geometry programs (`STAGE_GEOMETRY`)
A compute kernel, one thread per input primitive and invocation: `sp[3]` = invocations (SPH word 2 bits 24..31, up to 32), the kernel runs `sp[0] * sp[3]`
threads, thread = (primitive `gs_pid = gid / sp[3]`, invocation `inv = gid % sp[3]` = `SR_INVOCATION_ID`), and returns when past the count. Key:
`gs_max_vertices` (1..1024, required), `gs_in_vertices` (1, 2, 3, 4 lines+adjacency, 6 triangles+adjacency; default 3), `gs_out_topology` (`GS_OUT_*`).

| Buffer | Binding | Contents |
|---|---|---|
| `stage_in` | 19, `device const float4 *` | the vertex stage's `stage_out` records, assembled as a list: primitive p reads records `sp[1] + p * gs_in_vertices + v`. The replay expands strips / fans / indexed draws into this list |
| `sp` | 20, `constant uint *` | `[0]` input primitives, `[1]` first vertex record, `[2]` primitive id of thread 0, `[3]` invocations |
| `gs_vtx` | 21, `device float4 *` | output vertex records, `gs_max_vertices` per thread: record `gid * gs_max_vertices + n`. The kernel is keyed without `flip_y` / `depth_minus_one`: the pass-through vertex function applies them, so the records stay the program's own values for transform feedback |
| `gs_idx` | 22, `device uint *` | `2 * gs_max_vertices` indices per thread, pre-filled with `0xFFFFFFFF` (primitive restart) by the kernel. EMIT appends the vertex's record index, CUT appends one restart |
| constant buffers, textures, images, gmem, rscale | as every stage | |

The replay draws `gs_idx` (indexed, restart enabled, `gs_out_topology`) over `gs_vtx` with a pass-through vertex function that fetches the slots in
`stage_slot_mask`; the fragment shader reads the generic slots as ordinary varyings. `ALD a[v][addr]` reads input vertex `v` (the register in bits 39..46;
none = vertex 0, indexed reads select the slot at run time); attribute 0x60 reads the primitive id (it runs on across instances); `AST` fills the current
output vertex; `OUT` mode 1 = EMIT, 2 = CUT, 3 = both (stream operand ignored, stream 0 only). Output beyond `gs_max_vertices` is dropped. Per-primitive
layer / viewport index stores land in slot 0 (`.y` / `.z`); the pass-through vertex function routes them (see viewport arrays).

GLSLC geometry code reads its inputs through vertex handles: `S2R SR_INVOCATION_INFO` (0x1d), handle k = (info & 0xff) * ((info >> 16) & 0xff) + k,
`ISBERD`, `ALD a[handle][..]`, with handle 0x80000000 when k >= the vertex count. SR 0x1d answers vertex count << 16 | invocation in geometry (patch
vertices << 16 in tess control / eval), `ISBERD` in those stages turns a handle into the ALD vertex operand (geometry: modulo the vertex count), and a
geometry ALD whose vertex operand lies past the primitive reads 0.

### Tessellation
`ShaderKey.tess_out_vertices` (output control points = invocations per patch, 1..32), `tess_in_vertices` (control points of an input patch, tess control
only), `tess_domain` (`TESS_DOMAIN_QUAD` / `TRIANGLE`; isolines go through the software tessellator).

*Tess control (`STAGE_TESS_CTRL`)*: compute kernel, one threadgroup per patch (`sp[0]` threadgroups), `tess_out_vertices` threads per threadgroup;
`S2R SR_INVOCATION_ID` is the thread (output control point), `BAR` is a threadgroup barrier that also orders device memory. Inputs: `stage_in` (19, record
`sp[1] + patch * tess_in_vertices + v`) and `sp` (20, `[2]` first patch id). Outputs: `tess_vtx` (21, record `patch * tess_out_vertices + invocation`,
written by plain `AST`), `tess_patch` (22, patch record `patch`, written by `AST.P`), `tess_fac` (23, `device half *`). `ALD.O` reads another invocation's
output after a barrier; `ALD.P` reads the patch record. After a final barrier invocation 0 converts the level slots to Metal's factor structures: quad =
6 halves per patch (edge[0..3] = outer levels 0..3, inside[0..1] = inner levels 0..1, `MTLQuadTessellationFactorsHalf`), triangle = 4 halves (edge[0..2],
inside[0]; `MTLTriangleTessellationFactorsHalf`). A program that stores no level leaves `tess_fac` untouched, so the replay pre-fills it from the NVN levels
(`SetOuter/InnerTessellationLevels`; `ShaderInfo.patch_slot_mask` bits 0 / 1 tell). Without a tess control program the replay fills `tess_vtx`
(pass-through of the vertex records) and `tess_patch` / `tess_fac` itself.

*Tess eval (`STAGE_TESS_EVAL`)*: a post-tessellation vertex function `[[patch(quad|triangle, tess_out_vertices)]] vertex VOut main0(uint patch_id [[patch_id]],
float2|float3 tess_coord [[position_in_patch]], ...)` with `sp` (20), `tess_vtx` (21) and `tess_patch` (22) as `device const float4 *`; its outputs leave
through the normal vertex output struct (`flip_y` / `depth_minus_one` apply). Attributes 0x2F0 / 0x2F4 are `tess_coord.x / y`, attribute 0x60 is
`sp[2] + patch_id`, `ALD a[v]` reads control point v of the patch, `ALD.P` the patch record. The pipeline uses `maxTessellationFactor` 64, half factors per
patch, and the partition mode and winding from the tessellation parameters.

### Replay passes
A draw with a geometry or tessellation program, a `PATCHES` draw, or a draw while transform feedback is capturing runs as passes:
1. *Vertex capture*: the vertex program with `stage_out` (or the xfb entries) over the draw's vertex range in a 1x1 attachment-less pass, drawn as points with
   no fragment function (rasterization stays on: Metal requires a `void` vertex function when it is off, and the translated one returns `VOut`);
   `xfb_ctl = {range, lo + range * base_instance}` because `[[instance_id]]` includes the base instance.
2. *Assembly*: the CPU expands the draw (indices, base vertex, primitive restart; lists, strips with odd triangles' first two vertices swapped, fans, loops,
   quads, adjacency including triangle strips with adjacency per the GL 4.6 table 10.1, patches of `SetPatchSize`) into record ordinals and a gather kernel
   builds the list (`stage_in`, or `tess_vtx` without a tess control program).
3. *Geometry*: the kernel with `gs_in_vertices` from the draw mode and `gs_out_topology` / `gs_max_vertices` from the SPH (word 3 bits 24..27: 1 points,
   6 line strip, 7 triangle strip; word 4 bits 0..11), then `gs_idx` drawn as restart-separated strips (points pull their record through `gs_idx`; unused
   slots are placed outside the clip volume) by a generated pass-through vertex function. A geometry program with no `OUT` (pass-through) or one that does
   not translate is drawn without the geometry stage (logged once per program).
   *Tessellation*: `tess_out_vertices` from the tess control SPH (word 2 bits 24..31) or the patch size, the tess control kernel over `npatch` threadgroups,
   the factors pre-filled, then `drawPatches` with the tess eval pipeline.
4. *Transform feedback* without a geometry or tessellation stage: a non-indexed points / lines / triangles draw whose primitives all fit is captured by the
   vertex program itself; any other draw captures records and a copy kernel writes the decomposed primitives in order, stopping at whole primitives when a
   buffer is full. Byte counts are kept per buffer at encode time; the control memory given to Begin / End / Pause / Resume / DrawTransformFeedback uses
   Napple's own layout (u32 bytes[4], u32 stride[4]).

*Transform feedback of geometry output*: the vertex count a geometry program emits is only known on the GPU, so the first such capture moves the session's
counts to a 32-byte counter buffer (`xfb_go_gpu`): the serial `xfbgpu` kernel decomposes `gs_idx` into whole primitives, writes the layout's entries from
`gs_vtx` and advances the counters; later vertex-stage captures of the same session go through records and the same kernel. End / Pause blit the counters
into the control memory (the guest reads them after the submission completes, as on the console), Resume of a control memory last written by the GPU blits
it back, and `DrawTransformFeedback` of such a control memory draws indirectly (`xfbargs` kernel). Every capture keeps the program's own position values
(the clip-space conversions are in the pass-through vertex function, `stage_pipe_key.gs_clip`). The layout is looked up by the geometry program's hash, then
the vertex program's.

*Indirect draws with stages or during capture*: the arguments are read from guest memory at encode time (as indirect dispatches are): right when the game
wrote them before the submit, wrong for arguments a GPU pass of the same submission produces (logged once). `MultiDraw*IndirectCount` with stage programs:
`stage_draw_indirect_count` reads the count and records at encode time and sends each draw through `do_draw_indirect`; it takes effect once
`do_draw_indirect_count` (`mtl_replay.m`) calls it first (`t_stages` 2e, `-DNAPPLE_TEST_IND_COUNT_STAGES`).

Driver constants of the extra stages: the storage buffer table of hardware stage n is at c0 + 0x110 + 0x100 * n (VS 0, TCS 1, TES 2, GS 3, FS 4; VS and FS
measured, the others inferred from the spacing).

### Software tessellator
Metal's tessellator feeds the rasterizer only, so a `PATCHES` draw that needs more goes through a software tessellator in `mtl_stages.inc`: an isoline
domain, a draw while transform feedback captures tess eval output, or a geometry program after tessellation. Plain triangle / quad patches keep
`drawPatches`. Passes 1-2 and the tess control kernel are the same; then the `tessgen` kernel (one thread per patch) reads the factor record (quad layout
for isolines: outer[0] lines, outer[1] segments) and writes a fixed number of slots per patch: coordinates `float4(u, v, w, patch id)` and index strips
(3 + restart per triangle, 2 + restart per line segment, unused slots restarts; winding from the parameters). The tess eval program then runs as a plain
vertex function over those slots (`ShaderKey.tess_soft`: `vertex VOut main0(uint vid [[vertex_id]], ..., device const float4 *tess_uv [[buffer(19)]])`,
`TESS_UV_BUFFER_INDEX` reuses the `stage_in` slot), drawn as indexed line / triangle strips (`SPK_TESS_SOFT`) or into stage-out records with rasterization
off (`SPK_TESS_SOFT_CAPTURE`, `capture_void`). Capture: the `xfbgpu` kernel over the index strips (GPU-counted session). Geometry after tessellation: the
`tesslist` kernel packs the non-empty primitives into record ordinals and writes the geometry kernel's `sp` (count on the GPU), the output slots of threads
past the count are pre-filled as restarts, and `gs_run` does the rest.

Topology: isolines follow GL (lines at v = j / n0, j < n0); triangles and quads are a uniform grid (quad nu x nv cells from max(inner, the two parallel
outer levels), triangles a barycentric grid of the largest level): the hardware's vertices when all levels are equal with integer spacing, but not its
ring triangulation; fractional spacing rounds the segment count to the spacing's parity with evenly spaced vertices. Outer level <= 0 culls the patch.
Level cap: exact without a tess control program (the CPU knows the levels), `NAPPLE_GFX_SOFT_TESS_MAX` (default 16) with one (logged once). Indirect
`PATCHES` draws go through the same path. Not covered: point-mode tessellation, the exact hardware triangulation of unequal levels.

### Parameters from the control section
`gfx_shader_register` reads every stage's GLSLC control section (`gfx/src/shader/control.c`, `control.h`, `stages.h`; known fields: +0x20 length,
+0x6fc / +0x700 / +0x704 constant and code bounds, +0x714 stage, +0x748 compute group) and registers what it finds: a transform feedback layout for a
vertex / geometry / tess eval program (`gfx_xfb_set_layout`, keyed by that program's blob hash) and the tess eval parameters (`gfx_tess_set_params`).
Section lengths seen in real titles are 0x7d0/0x7d1, 0x848/0x850, 0x880 and up to 0x11d0; +0x030..+0x6ef is zero except in sections with embedded NV
assembly text (a fixed pattern at +0x034..+0x058 the consistency checks refuse); +0x718 is fragment state (1 / 0x10000) in fragment sections, so it is only
a candidate for tess eval sections.

**Encodings** (checked against NVIDIA's public Maxwell-B class header, open-gpu-doc `classes/3d/clb197.h`, layout facts only):
- Tessellation word `NVB197_SET_TESSELLATION_PARAMETERS` (0x0320): DOMAIN_TYPE 1:0 (isoline 0 / triangle 1 / quad 2), SPACING 5:4 (integer 0 /
  fractional odd 1 / fractional even 2), OUTPUT_PRIMITIVES 9:8 (points 0 / lines 1 / triangles CW 2 / triangles CCW 3). `gfx_control_tess` accepts lines
  only with the isoline domain and triangles only with triangle / quad, and refuses POINTS (point mode is not represented; the caller keeps its defaults).
- Transform feedback: `SET_STREAM_OUT_CONTROL_STRIDE(j)` (0x0708 + 16 j, bytes), `_COMPONENT_COUNT(j)` (0x0704 + 16 j, 8 bits) and
  `SET_STREAM_OUT_LAYOUT_SELECT(i, j)` (0x2800 + 128 i + 4 j, four 8-bit attribute numbers per word, low byte first) match
  `gfx_xfb_layout_from_varyings` (`u8 locs[4][128]` is the little-endian image of the 32 method words of a buffer; attribute number = address / 4).

**Hypotheses that remain.** Where these blocks sit inside the GLSLC control section is not proven: no scanned title has a tess control / tess eval program
or a transform feedback layout. `GLSLC_XFB_*` (+0x30: u32 stride[4], u32 count[4], u8 locs[4][128]) and `GLSLC_TESS_MODE_OFF` (+0x718) are therefore read
only when the bytes there are non-zero and self-consistent; a scan of real control sections reported no false positive. Explicit registrations
(`gfx_program_set_*`, keyed by the stage that feeds the rasterizer: geometry, else tess eval, else vertex) still work and overwrite. Without a layout nothing
is captured (one warning per program); without parameters tessellation uses triangles / equal spacing / CCW (`NAPPLE_GFX_TESS_DOMAIN=quad` changes the domain).

**Evidence lines.** `NAPPLE_GFX_CTL_DUMP=<dir>` writes every registered control section and code area; the boot log says once when a title creates a
geometry / tess control / tess eval program; `stage path ran: geometry | tessellation | software tessellation, program <hash> ...` is logged once per
program and the running count at powers of two. `tools/gpu/scan_stages.py` finds these programs in a title without running it ([guide](../guides/gpu-title-scan.md)).

### Stage instructions
`OUT` (geometry only; elsewhere refused with "OUT: geometry shader output outside a geometry shader"), `VABSDIFF4`, `TXA`, `IMADSP`. Their bit layouts are
the translator's reading, written down in the comments at the decoder branches; `tests/t_xlate_stages.inc` pins them (tested, not verified against hardware
words). `TXA` is read as `TXQ` (selector 1 = dimensions and mip count; other selectors are refused by name).

## Texture views and bindings

- **Integer sampled textures.** `ShaderKey.tex_uint_slots` / `tex_sint_slots` (depth_slots numbering) declare a binding as an integer texture read through
  wrappers whose read / nearest sample return the four channels as raw integer bits (signed formats sign-extended): `IntTex2D`, `IntTex2DArray`,
  `IntTex3D`, `IntTexCube` (face and texel from the direction), `IntTex2DMS`, `IntTex1D`, `IntTex1DArray`, `IntTexCubeArray`, `IntTex2DMSArray`. Only texel
  buffers keep the float declaration. The wrappers' `calculate_*_lod` return a float, as Metal's do. `mtl_tex_int_kind` gives a binding's kind; the masks
  are filled by `int_slot_masks` in `mtl_replay.m` / `mtl_stages.inc`; integer views of another format class become copies (for example RG32UI of BC1).
- **Stencil sampling.** `ShaderKey.stencil_slots` (same numbering as `depth_slots`, wins over it) marks bindings whose bound view is in stencil mode. Such
  a binding is declared `texture2d<uint>` / `texture2d_array<uint>` / `texturecube<uint>` / `texturecube_array<uint>` and wrapped in `StencilTex2D` /
  `StencilTex2DArray` / `StencilTexCube` / `StencilTexCubeArray`, whose `sample` / `gather` / `read` return `(stencil, 0, 0, 1)` as raw integer bits (level 0,
  nearest; for cubes the direction selects the face with the GL / Metal convention, `cube_face`). Depth comparison on a stencil view is refused. The replay
  sets the bit when `mtl_tex_view` has `ds_mode` stencil and binds the X32_Stencil8 / Stencil8 view.
- **Cube view of a 2D texture.** A CUBEMAP view of a square 2D texture is a copy-backed cube (`AK_CUBE`) with the 2D image on every face (the console reads
  unrelated memory for faces 1-5).
- **Separate texture and sampler handles.** The replay keeps `nvnCommandBufferBindSeparateTexture(s)` / `Sampler(s)` per stage (`S.septex` / `S.sepsamp`).
  The translator turns a bindless TEX whose handle register is (constant buffer word) | (constant buffer word) (LOP.OR, IADD, LOP3 with an OR table) into
  `ShaderTexture.separate` with both words, and `bind_stage` ORs them, each from c2 (combined table at 0x20, separate textures at `SEP_TEX_C2_BASE` 0x120,
  separate samplers at `SEP_SAMP_C2_BASE` 0x520, 8 bytes per binding) or from a user uniform buffer. `ShaderKey.sep_depth_slots` / `sep_stencil_slots` /
  `sep_uint_slots` / `sep_sint_slots` mark separate texture binding k (c2 `SEP_TEX_C2_BASE` + 8k, k < 32) as the combined masks mark combined slots; the
  replay fills them from the bound separate textures (`sep_slot_masks`, also for geometry / tess stages through `stage_key_masks`), and a word from a user
  uniform buffer gets none. The c2 placement of the separate tables is a **hypothesis** (no title seen binds separate handles; logged once with the offsets a
  shader uses, for calibration).
- **Sparse residency.** `nvnMemoryPoolMapVirtual` keeps the textures of the remapped range (`mtl_tex_remap_scope`), a sparse texture never becomes
  `gpu_owned`, and a residency change re-uploads only the 64 KiB tiles whose pages changed (`upload_texture_part`; later levels whole).
- **Copies.** `CopyTextureToTexture` with a different size or format is a draw (format conversion and scaling); depth / MSAA / 3D variants of that are not
  implemented. A texture-to-buffer copy of DEPTH24 (0x33) / DEPTH24_STENCIL8 (0x35), held as 32-bit float depth (+ stencil), blits the aspects to staging
  and a kernel packs NVN's words (`copy_d24_from_tex`: unorm24 in the low bits, or depth << 8 | stencil), the exact inverse of the upload. Buffer <-> texture
  copies of a 1D array take the region's y / height as layers (as in GL).
- **Clears.** A clear folded into the next pass's load action is applied before any later readback (an open blit encoder is ended first). `ClearTexture`
  with a region smaller than the level or a channel mask other than 0xf draws a clear quad (`clear_tex_pipe`, cached per format / kind / mask / sample count)
  into each addressed layer under a scissor with the colour write mask; a mask of 0 writes nothing; 3D, 1D and 1D-array targets and depth formats clear the
  whole subresource.

## Raster state

- **Viewport arrays.** Every draw sets all 16 viewports, depth ranges and scissors on the encoder (`setViewports:count:16`, `setScissorRects:count:16` in
  `apply_raster_state`), each converted as viewport 0 (lower-left origin, render target scale). An unset or empty viewport covers the target; an unset
  scissor (`S.sc_set`, one bit per scissor) covers the target. Every scissor is clamped to Metal's render pass size (`P.pw` / `P.ph`: the smallest attachment
  at its level), which is smaller than the guest size in a pass into a mip level. A vertex / tess eval program that writes the viewport index (0x68)
  declares `uint vpi [[viewport_array_index]]` (clamped to 15); a geometry program's index (slot 0 `.z`) goes out through the pass-through vertex function
  (`stage_pipe_key.gs_sys`, `GS_SYS_VP_INDEX`). The arrays are set again only when they changed in the encoder (`g_vp_serial`).
- **Layered rendering.** A program that writes the layer (0x64, or slot 0 `.y` of a geometry program) declares `uint layer [[render_target_array_index]]`
  only when the pass is layered (`ShaderKey.layered`, `pipe_key.layered`), because Metal rejects that output in a pass without `renderTargetArrayLength`. The
  pass is layered on demand: the first such draw in a pass whose attachments all have two or more layers from their slice (2D array, 2D multisample array,
  cube, cube array; limited by the view's layer count) ends the pass and reopens it with `renderTargetArrayLength` = that count (`pass_routing`), and the
  pipeline is looked up again with the layered key. The layer is relative to the attachment's slice. With a single-image attachment the layer is dropped and
  the primitive lands in the attachment's own image, as in GL. Clears folded into a pass's load action clear the attachment's own slice only; whether NVN
  clears every layer of a layered target is not checked. `ShaderInfo.writes_vp_index` / `writes_layer` say what a program stores; the first draw of each
  kind is logged (`viewport index written (program ...)`, `layer written (program ...)`).
- **Viewport swizzles.** `nvnCommandBufferSetViewportSwizzles` is recorded (`OP_VP_SWIZZLE`, four `NVNviewportSwizzle` per viewport). Viewport 0's swizzle
  (`ShaderKey.vp_swizzle`, `pipe_key.vp_swizzle`, XOR `VP_SWIZZLE_IDENTITY` so 0 is the identity) is applied to the clip position of the stage that feeds the
  rasterizer (vertex, tess eval, the geometry pass-through), after transform feedback capture and before the flip / depth conversions. A program that writes
  the viewport index selects its viewport's swizzle from a 16-entry table at run time (`ShaderKey.vp_swizzle_vp`, `maxwell_vp_swizzle_msl`); capture-only
  passes apply none.
- **Provoking vertex.** `nvnCommandBufferSetProvokingVertexMode` is recorded (`OP_PROVOKING`). Metal takes flat outputs from the first vertex, which is
  mode 0. Mode 1 (last) re-emits direct draws as lists with each primitive's last vertex first (`pv_last_indices`: triangles rotated, which keeps the
  winding; lines reversed; strips, loops, fans, quads and quad strips split; polygons keep their first vertex; primitive restart honoured). Indirect draws
  and geometry / tess draws keep the first vertex (logged once). The 0 = first, 1 = last mapping follows the Maxwell register because the NVN enum is not
  public: a **hypothesis**.

## Rare control and system instructions

`BPT IDE RAM SAM STP R2B` are documented no-ops, `RTT` ends the thread, `B2R` writes 0, `PEXIT` (the next `EXIT` jumps to its target),
`PLONGJMP`/`LONGJMP` (a second PBK/BRK-style stack), `GETCRSPTR`/`SETCRSPTR` (the CAL depth `cal_n`), `GETLMEMBASE`/`SETLMEMBASE` (a readable `lmb`
variable), `ISBERD` (register-addressed attribute read, the ALD indexed path) and `IDP` (`idp()` helper: 4 byte or 2 half dot product + accumulate) are
translated; their field layouts are the translator's reading (`tests/t_xlate_rare.inc`). Other translator facts: `LEA.HI` constant-buffer form (opcode
000110), `TEX` AOFFI on 3D textures (`int3`) and none on 1D / cube, `TLDS` texture_info 6 (2D.LZ.MS), `TXQ` / `TXA` through `txq_w` / `txq_h` / `txq_n`
overloads (a later TLDS.MS makes the binding multisampled), `TLD4.PTP` as four gathers (texel i = the base texel of the gather at offset i). How to add
an instruction: [add-maxwell-shader-op.md](../guides/add-maxwell-shader-op.md).

## Tests

`make -C gfx test` builds `libnapple_gfx.a` and every `gfx/tests/t_*` program and runs them with `MTL_DEBUG_LAYER=1`. On Linux, `sh gfx/tests/t_xlate_linux.sh`
checks the translator text without Metal.

| Area | Tests |
|---|---|
| Translator | `t_xlate` (every case compiles with Metal on macOS; corpus mode with `NAPPLE_SHADER_CORPUS=<dir>`, see [gpu-title-scan.md](../guides/gpu-title-scan.md)), `t_xlate_stages.inc`, `t_xlate_rare.inc`, `t_control`, `t_tess_xfb_layout` (all 36 domain x spacing x primitive words, the packing, the section block) |
| Stages | `t_stages` (exact captured words, control memory, pixels; 2e indirect count, 2f GLSLC vertex handles, 4a-4d software tessellator), `t_stages_dumped` (a title's dumped geometry programs, `NAPPLE_GFX_STAGE_DUMP`), `t_multi_viewport`, `t_vp_swizzle`, `t_provoking` |
| Views and copies | `t_image`, `t_int_tex`, `t_stencil_sample`, `t_sep_sampler`, `t_d24_readback`, `t_feedback`, `t_clear_ds`, `t_clear_tex` |
| Objects and recording | `t_obj_canary`, `t_cmdshare`, `t_cmdlife` |
| Display | `t_scale_math` (factors, allocation sizes, viewport and scissor conversion, rscale entries, budget, fit rectangles, upscaler fallback), `t_scale_upscale` (upscaler matrix with PSNR and sharpness asserts, writes a contact sheet), `t_scale_table`, `t_display` |

Real games: the golden-frame gate ([testing-graphics.md](../guides/testing-graphics.md)) and `tools/golden/render_scale.py shots|stress` (headless drawable
size from `NAPPLE_OUTPUT_SIZE=WxH`).

## Environment variables

The full list is in [environment-variables.md](../reference/environment-variables.md). The ones specific to the mechanisms above:

| Variable | Effect |
|---|---|
| `NAPPLE_REC_BUDGET_MB`, `NAPPLE_REC_NO_PACK` | safety ceiling for live recordings; disable LZ4 packing of idle lists |
| `NAPPLE_GFX_CBTRACE`, `NAPPLE_GFX_CMDTRACE`, `NAPPLE_NVNTRACE` | command-buffer race trace; recording lifetimes; op number of each recorded command |
| `NAPPLE_GFX_OBJTRACE` | guest object size measurement |
| `NAPPLE_GFX_NO_ARCHIVE`, `NAPPLE_GFX_ARCHIVE_MAX_MB`, `NAPPLE_GFX_NO_PREWARM`, `NAPPLE_GFX_PIPE_SKIP`, `NAPPLE_GFX_SPECULATE` | pipeline caches and compile policy |
| `NAPPLE_SHADER_DUMP`, `NAPPLE_SHADER_DUMP_HASH`, `NAPPLE_SAVE_MSL`, `NAPPLE_GFX_MATH` | shader dumps, annotated MSL, MSL math mode (`safe` default, `relaxed`, `fast`) |
| `NAPPLE_GFX_CTL_DUMP`, `NAPPLE_GFX_STAGE_DUMP` | dump control sections and code; run dumped geometry programs in `t_stages_dumped` |
| `NAPPLE_GFX_NO_STAGES`, `NAPPLE_GFX_SOFT_TESS_MAX`, `NAPPLE_GFX_TESS_DOMAIN` | geometry / tessellation path |
| `NAPPLE_SCALE_DIAG`, `NAPPLE_RSCALE_TABLE` | render-scale decisions; per-title scale table file |
| `NAPPLE_DEBUG_SKIP_VS`, `NAPPLE_SHADER_PROBE`, `NAPPLE_DRAWLOG*` | bisecting and inspecting draws ([debugging.md](../guides/debugging.md)) |
