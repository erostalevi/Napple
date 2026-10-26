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
- Command handles outlive their command buffer (graveyard of 1024 recordings), required by Super Mario Odyssey.
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

**Calls the Mac glue needs** (`gfx/src/mtl_shader.m`, `mtl_replay.m`, owned by gfx-correctness; none of this exists yet):

1. After `maxwell_translate`, bind each `info->img[i]` texture with `setTexture:atIndex:IMAGE_TEXTURE_BASE + i` on the compute / fragment / vertex encoder: resolve the handle from c2 at `handle_offset` (or the bound user buffer at `bindless_cbuf` / `bindless_offset`) through the same TIC lookup as sampled textures.
2. Make the texture a view with `MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite` (plus `ShaderAtomic` when `img[i].atomic`) and an integer pixel format when `kind` is `INT` / `UINT`; sRGB views cannot be written, use the linear twin.
3. Fill `ShaderKey.image_int_mask` / `image_uint_mask` from the formats of the bound images when building the pipeline key (pipelines for the same shader with different formats are different pipelines).
4. Compile with `MTLLanguageVersion3_1` when `needs_msl31`.
5. `useResource:usage:` is not needed unless the images are reached through an argument buffer.
6. Tell the dirty-tracker (`dirty.c`) that a shader-written image modifies guest memory (write-back to the guest pages).
